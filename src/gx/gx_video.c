#include "gx/msl_gx.h"

#if MSL_GX_RECORD

/* The game's video and GX bring-up and its frame end, as the GX commands they write: the bodies are the decomp's
 * (refs/melee/src/sysdolphin/baselib/{video.c,initialize.c}), without the VI hardware, the interrupts and the
 * frame buffers. */

#include <string.h>

#include <baselib/initialize.h>
#include <baselib/lobj.h>
#include <baselib/state.h>
#include <baselib/video.h>
#include <dolphin/gx.h>

extern GXRenderModeObj GXNtsc480IntDf;

HSD_VIInfo HSD_VIData;
static u8 garbage[HSD_ANTIALIAS_GARBAGE_SIZE] ATTRIBUTE_ALIGN(32);
static HSD_RenderPass current_render_pass;
static int current_pix_fmt;
static int current_z_fmt = GX_ZC_MID;
static GXColor HSD_Init_804D5E1C = { 0 };

#define _p ((HSD_VIInfo*) &HSD_VIData)

/* video.c */
static void HSD_VICopyEFB2XFBHiResoAA(GXRenderModeObj* rmode)
{
    int n_xfb_lines;

    GXSetDispCopySrc(0, 0, rmode->fbWidth,
                     rmode->efbHeight - HSD_ANTIALIAS_OVERLAP);
    n_xfb_lines = GXSetDispCopyYScale(1.0);
    GXSetDispCopyDst(rmode->fbWidth, n_xfb_lines);
}

void HSD_VICopyEFB2XFBPtr(HSD_VIStatus* vi, void* buffer, HSD_RenderPass rpass)
{
    GXRenderModeObj* rmode = &vi->rmode;
    int n_xfb_lines;
    u16 lines;
    u32 offset;

    GXSetCopyFilter(rmode->aa, rmode->sample_pattern, vi->vf, rmode->vfilter);
    GXSetDispCopyGamma(vi->gamma);

    HSD_StateSetColorUpdate(vi->update_clr);
    HSD_StateSetAlphaUpdate(vi->update_alpha);
    HSD_StateSetZMode(vi->update_z, GX_LEQUAL, GX_TRUE);

    GXSetCopyClear(vi->clear_clr, vi->clear_z);

    switch (rpass) {
    case HSD_RP_SCREEN:
        GXSetCopyClamp((GXFBClamp) (GX_CLAMP_TOP | GX_CLAMP_BOTTOM));
        GXSetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
        n_xfb_lines = GXSetDispCopyYScale((f32) (rmode->xfbHeight) /
                                          (f32) (rmode->efbHeight));
        GXSetDispCopyDst(rmode->fbWidth, n_xfb_lines);
        GXCopyDisp(buffer, GX_TRUE);
        break;

    case HSD_RP_TOPHALF:
        HSD_VICopyEFB2XFBHiResoAA(rmode);
        GXSetCopyClamp(GX_CLAMP_TOP);
        lines = rmode->efbHeight - HSD_ANTIALIAS_OVERLAP;
        GXSetDispCopySrc(0, 0, rmode->fbWidth, lines);
        GXCopyDisp(buffer, GX_TRUE);
        GXPixModeSync();
        return;

    case HSD_RP_BOTTOMHALF:
        HSD_VICopyEFB2XFBHiResoAA(rmode);
        GXSetCopyClamp(GX_CLAMP_BOTTOM);
        lines = rmode->efbHeight - HSD_ANTIALIAS_OVERLAP;
        GXSetDispCopySrc(0, HSD_ANTIALIAS_OVERLAP, rmode->fbWidth, lines);
        offset = 640 * lines * 2;
        GXCopyDisp((void*) ((u8*) buffer + offset), GX_TRUE);
        GXSetDispCopySrc(0, 0, rmode->fbWidth, HSD_ANTIALIAS_OVERLAP);
        GXSetCopyClamp((GXFBClamp) (GX_CLAMP_TOP | GX_CLAMP_BOTTOM));
        GXCopyDisp((void*) garbage, GX_TRUE);
        break;

    default:
        break;
    }

    if (rpass != HSD_RP_SCREEN) {
        GXPixModeSync();
    }
}

/* initialize.c */
static void HSD_GXInit(void)
{
    GXLightObj lightobj;
    int i;

    GXInitLightPos(&lightobj, 1, 0, 0);
    GXInitLightDir(&lightobj, 1, 0, 0);
    GXInitLightAttn(&lightobj, 1, 0, 0, 1, 0, 0);
    GXInitLightColor(&lightobj, HSD_Init_804D5E1C);

    for (i = 0; i < 8; i++) {
        GXLoadLightObjImm(&lightobj, HSD_Index2LightID(i));
    }
    HSD_StateInvalidate(-1);
}

GXRenderModeObj* msl_gx_video_rmode(void)
{
    return &HSD_VIData.current.vi.rmode;
}

/* the data cache: the host has one memory */
void DCFlushRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void DCStoreRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void DCInvalidateRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void HSD_Init_803755A8(void)
{
    /* (does nothing in the game either) */
}

HSD_RenderPass HSD_GetCurrentRenderPass(void)
{
    return current_render_pass;
}

void HSD_StartRender(HSD_RenderPass pass)
{
    GXRenderModeObj* rmode = HSD_VIGetRenderMode();
    current_render_pass = pass;
    if (rmode->aa) {
        GXSetPixelFmt(GX_PF_RGB565_Z16, current_z_fmt);
    } else {
        GXSetPixelFmt(current_pix_fmt, GX_ZC_LINEAR);
    }
    GXSetFieldMode(rmode->field_rendering, rmode->xfbHeight < rmode->viHeight);
}

/* HSD_InitComponent's and HSD_VIInit's part in the GX stream: the render mode, then one copy of the frame buffer
 * (the black frame the console shows first) and the eight lights set to nothing. */
void msl_gx_video_init(void)
{
    HSD_VIStatus vi_status;
    GXColor black = { 0, 0, 0, 0 };
    int idx;

    memset(&vi_status, 0, sizeof(vi_status));
    vi_status.rmode = GXNtsc480IntDf;
    vi_status.black = GX_TRUE;
    vi_status.vf = GX_TRUE;
    vi_status.gamma = GX_GM_1_0;
    vi_status.clear_clr = black;
    vi_status.clear_z = GX_MAX_Z24;
    vi_status.update_clr = GX_ENABLE;
    vi_status.update_alpha = GX_ENABLE;
    vi_status.update_z = GX_ENABLE;

    memset(&HSD_VIData, 0, sizeof(HSD_VIData));
    _p->current.vi = vi_status;
    _p->current.chg_flag = 0;
    _p->nb_xfb = 2;
    for (idx = 0; idx < 2; ++idx) {
        _p->xfb[idx].status = HSD_VI_XFB_FREE;
        _p->xfb[idx].vi_all = _p->current;
    }
    _p->efb.status = HSD_VI_EFB_FREE;
    _p->efb.vi_all = _p->current;
    HSD_VICopyEFB2XFBPtr(HSD_VIGetVIStatus(), NULL, HSD_RP_SCREEN);
    HSD_GXInit();
}

/* gmscene.c's end of a frame, HSD_VICopyXFBAsync(HSD_RP_SCREEN): the copy of the frame buffer ends the frame (a recording
 * cuts there); the commands after it (GXPixModeSync, the draw-done token) open the next one. */
void msl_gx_video_end_frame(void)
{
    HSD_VICopyEFB2XFBPtr(HSD_VIGetVIStatus(), NULL, HSD_RP_SCREEN);
}

void msl_gx_video_frame_tail(void)
{
    GXPixModeSync();
    GXSetDrawDone();
}

#endif /* MSL_GX_RECORD */

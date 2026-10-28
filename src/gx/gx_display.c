#include "gx/msl_gx.h"

#if MSL_GX_RECORD

/* The display pass of a frame: the game's own drawing code run into the GX layer's pipe (src/gx/msl_gx.h). Each frame,
 * after the simulation has stepped, this does what the game does between its game logic and the swap
 * (refs/melee/src/melee/gm/gmscene.c: lb_800195D0, GXInvalidateVtxCache, GXInvalidateTexAll, HSD_StartRender,
 * HSD_GObj_80390FC0, HSD_VICopyXFBAsync), then keeps the commands when a recording is on.
 *
 *   MSL_GX_RECORD_OUT=<file.dff>   write a Dolphin FIFO recording of the frames below
 *   MSL_GX_RECORD_FRAMES=a:b       the tape frame indexes a up to (not including) b; every earlier frame is drawn
 *                                  too (the GPU's state carries from frame to frame) but not kept
 *   MSL_GX_DUMP=1                  list the GObjs with a render callback, once, at frame a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <baselib/gobj.h>
#include <baselib/initialize.h>
#include <baselib/jobj.h>
#include <dolphin/gx.h>

#include "runtime/context.h"
#include "runtime/match.h"

int msl_gx_trace;  /* MSL_GX_TRACE=1: bring-up notes on stderr during the recorded frames */
int msl_w4_cur_step;   /* the tape frame the next step will run */
int msl_gx_skip;   /* MSL_GX_SKIP bits: 1 the drop shadows, 2 the fighters (bring-up switches) */
static int cfg_ready;
static int cfg_on;
static int cfg_first;
static int cfg_last;
static char cfg_out[1024];
static int cfg_dump;
static int cfg_hash;      /* MSL_GX_RECORD_OUT=hash: a hash of each frame's stream on stderr (GXHASH), nothing kept */
static int cfg_discard;   /* MSL_GX_RECORD_OUT=discard: the display pass runs every frame, nothing is kept (gameplay identity checks) */

static void read_config(void)
{
    const char* out = getenv("MSL_GX_RECORD_OUT");
    const char* frames = getenv("MSL_GX_RECORD_FRAMES");
    cfg_ready = 1;
    if (out == NULL || out[0] == '\0' || frames == NULL) {
        return;
    }
    if (sscanf(frames, "%d:%d", &cfg_first, &cfg_last) != 2) {
        fprintf(stderr, "msl_gx: MSL_GX_RECORD_FRAMES wants a:b\n");
        return;
    }
    snprintf(cfg_out, sizeof(cfg_out), "%s", out);
    cfg_discard = strcmp(cfg_out, "discard") == 0;
    cfg_hash = strcmp(cfg_out, "hash") == 0;
    cfg_dump = getenv("MSL_GX_DUMP") != NULL;
    if (getenv("MSL_GX_SKIP") != NULL) {
        msl_gx_skip = atoi(getenv("MSL_GX_SKIP"));
    }
    cfg_on = 1;
}

static void dump_gobjs(void)
{
    int link;
    int max = HSD_GObjLibInitData.gx_link_max + 1;
    for (link = 0; link <= max; ++link) {
        HSD_GObj* g;
        for (g = HSD_GObjGXLinkHead[link]; g != NULL; g = g->next_gx) {
            fprintf(stderr, "gxlink %2d prio %3d gobj %p class %u cb %p hsd_obj %p prios %llx\n", link, g->render_priority,
                    (void*) g, (unsigned) g->classifier, (void*) g->render_cb, g->hsd_obj,
                    (unsigned long long) g->gxlink_prios);
        }
    }
}

static void display_pass(MslCoreMatch* match)
{
    (void) match;
    /* the previous frame's last commands (the copy ended it) */
    msl_gx_video_frame_tail();
    /* refs/melee/src/melee/gm/gmscene.c */
    GXInvalidateVtxCache();
    GXInvalidateTexAll();
    HSD_StartRender(HSD_RP_SCREEN);
    HSD_GObj_80390FC0();
    HSD_Init_803755A8();
    msl_gx_video_end_frame();
}

void msl_gx_frame(MslCoreMatch* match, int index)
{
    msl_w4_cur_step = index + 1;
    if (!cfg_ready) {
        read_config();
    }
    if (!cfg_on || index >= cfg_last) {
        return;
    }
    if (!msl_gx_ready()) {
        msl_gx_init();
    }
    {
        extern void msl_w4_inject_dynamics(const MslCoreMatch* match, int frame);
        msl_w4_inject_dynamics(match, index);
    }
    msl_gx_frame_begin();
    {
        extern void msl_gx_trace_frame_mark(int index);
        msl_gx_trace_frame_mark(index);
    }
    msl_gx_trace = (getenv("MSL_GX_TRACE") != NULL) && index >= cfg_first;
    if (index == cfg_first && cfg_hash) {
        msl_gx_set_hash_mode(1);
        msl_gx_recording_start(cfg_out);
    } else if (index == cfg_first && !cfg_discard) {
        msl_gx_recording_start(cfg_out);
        if (getenv("MSL_W4_CHECK") != NULL) {
            extern void msl_w4_check_structure(const MslCoreMatch* match);
            msl_w4_check_structure(match);
        }
        if (cfg_dump) {
            dump_gobjs();
        }
    }
    {
        extern void msl_w4_poslog_frame(const MslCoreMatch* match, int frame);
        msl_w4_poslog_frame(match, index);
    }
    {
        extern void msl_w4_hide(const MslCoreMatch* match);
        msl_w4_hide(match);
        {
            extern void msl_w4_bump(const MslCoreMatch* match, int frame);
            msl_w4_bump(match, index);
        }
    }
    {
        extern int msl_viewer_display_skipped(int index);
        if (!msl_viewer_display_skipped(index)) {
            display_pass(match);
        }
    }
    {
        extern void msl_present_dump_particles_post(int step);
        msl_present_dump_particles_post(index);
    }
    msl_gx_trace = 0;
    msl_gx_frame_end();
    {
        extern void msl_w4_check_anims(const MslCoreMatch* match, int frame);
        msl_w4_check_anims(match, index);
        {
            extern void msl_w4_dump_dynamics(const MslCoreMatch* match, int frame);
            msl_w4_dump_dynamics(match, index);
        }
        {
            extern void msl_w4_dump_dynamics_all(const MslCoreMatch* match, int frame);
            msl_w4_dump_dynamics_all(match, index);
        }
        {
            extern void msl_w4_dump_all_joints(const MslCoreMatch* match, int frame);
            msl_w4_dump_all_joints(match, index);
        }
        {
            extern void msl_w4_dump_joints(const MslCoreMatch* match, int frame);
            msl_w4_dump_joints(match, index);
        }
        {
            extern void msl_w4_dump_item_joints(const MslCoreMatch* match, int frame);
            msl_w4_dump_item_joints(match, index);
        }
    }
    if (getenv("MSL_W4_CHECK") != NULL) {
        extern void msl_w4_check_fighters(const MslCoreMatch* match, int frame);
        msl_w4_check_fighters(match, index);
        {
            extern void msl_w4_check_items(const MslCoreMatch* match, int frame);
            msl_w4_check_items(match, index);
        }
    }
    if (index == cfg_last - 1 && !cfg_discard && !cfg_hash) {
        if (msl_gx_recording_finish() != 0) {
            fprintf(stderr, "msl_gx: could not write %s\n", cfg_out);
        }
    }
}

/* One frame for a caller that streams (the browser build, tools/replay/gx_wasm_api.c): the display pass into a recording that keeps only
 * its last frame (msl_gx_kept). `first` starts that recording. */
void msl_gx_stream_frame(MslCoreMatch* match, int first)
{
    if (!msl_gx_ready()) {
        msl_gx_init();
    }
    msl_gx_frame_begin();
    if (first) {
        msl_gx_set_stream_mode(1);
        msl_gx_recording_start("");
    }
    display_pass(match);
    msl_gx_frame_end();
}

/* MSL_GX_SKIP_EVAL: see msl_viewer_skip_eval_frame (scalar.c): the display pass of a frame the console did not render is left out too (an empty frame is recorded) */
int msl_viewer_display_skipped(int index)
{
    const char* v = getenv("MSL_GX_SKIP_EVAL");
    while (v != NULL && *v != 0) {
        char* end;
        long f = strtol(v, &end, 10);
        if (end == v) {
            break;
        }
        if ((int) f == index) {
            return 1;
        }
        v = *end == ',' ? end + 1 : end;
    }
    return 0;
}

#endif /* MSL_GX_RECORD */

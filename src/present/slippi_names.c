/* Viewer build only (MSL_GX_RECORD): Slippi playback's "Show Player Names" (slippi-ssbm-asm Playback/Show Player Names/InitInGame.asm,
 * injected at 0x8016E9B4, the end of gm_Scene_Vs_OnEnter): under each player's HUD a dark box and the player's Slippi display name, drawn
 * with the game's text engine. The names come from the replay (Game Start event, 0x1A5 + 0x1F * port, Shift JIS), which the playback
 * buffer holds at PDB_DISPLAY_NAMES (Playback/Core/RestoreGameInfo.asm). This is the asm, step for step, with the game's functions it calls.
 * An empty translation unit without the flag.
 *
 * refs/slippi-ssbm-asm/Playback/Show Player Names/InitInGame.asm, Playback/Core/RestoreGameInfo.asm, Playback/Playback.s */
// present-include: melee/if
#if MSL_GX_RECORD

#include <stdlib.h>
#include <string.h>

#include "gm_names.h"

#include <melee/if/ifall.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/pl/player.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjobject.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>

/* PDB_DISPLAY_NAMES: string (31)[4] */
#define DISPLAY_NAME_BYTES 31
static unsigned char display_names[4][DISPLAY_NAME_BYTES];

void msl_present_clear_display_names(void)
{
    memset(display_names, 0, sizeof(display_names));
}

void msl_present_set_display_name(int port, const unsigned char* sjis31)
{
    if (port >= 0 && port < 4) {
        memcpy(display_names[port], sjis31, DISPLAY_NAME_BYTES);
        display_names[port][DISPLAY_NAME_BYTES - 1] = 0;
    }
}

/* the data block of the asm (DATA_BLRL) */
#define PLAYERBG_OPA 0.33F
#define PLAYERBG_YSCALE 0.62F
#define PLAYERBG_XOFST 0.775F
#define PLAYERBG_YOFST -24.06F
#define PLAYERBG_XSCALEMULT 0.0146F
#define PLAYERTEXT_XPOS 0.8F
#define PLAYERTEXT_YPOS 20.64F
#define PLAYERTEXT_ZPOS 0.0F
#define PLAYERTEXT_CANVASSCALE 0.06F
#define PLAYERTEXT_WIDTH 150.0F
#define PLAYERTEXT_SIZE 0.54F

#define COBJ_GXPRI 8
#define TEXT_GXPRI 80
#define TEXT_GXLINK 12
#define CHAR_WIDTH_MAX 20
#define TAG_WIDTH_MIN 60
#define TAG_WIDTH_MAX 144
#define TEXTHEADER_SIZE 15

/* ifnametag.c: un_804A1ED0.joint (0x804A1ED0, the name tag's box model); sislib.c: HSD_SisLib_8040CB00 (the default font's widths) */
extern HSD_Joint* msl_present_nametag_joint(void);
extern const unsigned char* msl_sislib_internal_widths(void);

/* COBJ_CB: the camera draws unless the game is paused (the byte at r13 - 0x4934; a match that draws is not paused) */
static void names_cobj_cb(HSD_GObj* gobj, int code)
{
    HSD_GObj_803910D8(gobj, code);
}

void msl_slippi_show_player_names(void)
{
    SceneDesc* scene;
    HSD_CObjDesc* desc;
    HSD_CObj* cobj;
    HSD_GObj* cobj_gobj;
    s32 canvas;
    int count;
    HSD_Archive** parchive = ifAll_802F3690();

    /* Get HUD CObjDesc (the IfAll archive, "ScInfDmg_scene_data"), create CObj and GObj */
    lbArchive_LoadSections(*parchive, (void**) &scene, "ScInfDmg_scene_data", (void*) 0);
    desc = scene->cameras[0].desc;
    cobj = HSD_CObjLoadDesc(desc);
    cobj_gobj = GObj_Create(19, 20, 0);
    HSD_GObjObject_80390A70(cobj_gobj, HSD_GObj_804D784B, cobj);
    GObj_SetupGXLinkMax(cobj_gobj, names_cobj_cb, COBJ_GXPRI);
    cobj_gobj->gxlink_prios = 1 << TEXT_GXLINK;

    /* Create canvas */
    canvas = HSD_SisLib_803A611C(2, cobj_gobj, 9, 13, 0, TEXT_GXLINK, TEXT_GXPRI, COBJ_GXPRI);

    for (count = 0; count < 6; count++) {
        const char* name;
        HSD_Text* text;
        HSD_GObj* bg_gobj;
        HSD_JObj* bg_jobj;
        HSD_JObj* joint1 = NULL;
        Vec3* hud;
        float hud_x;
        int width = 0;
        u8* cur;
        s32 unused_size = 0;

        if (Player_GetPlayerSlotType(count) == 3) {
            continue;
        }
        if (count >= 4 || display_names[count][0] == 0) {
            continue;
        }
        name = (const char*) display_names[count];

        /* Calculate X Position: the HUD position of the player */
        hud = ifAll_802F3424(count);
        hud_x = hud->x;

        /* the player text struct */
        text = HSD_SisLib_803A6754(2, canvas);
        text->default_fitting = 1;   /* Fixed Width */
        text->default_alignment = 1; /* align center */
        text->x4C = 1;
        text->default_kerning = 1;
        text->font_size.x = PLAYERTEXT_CANVASSCALE;
        text->font_size.y = PLAYERTEXT_CANVASSCALE;
        text->pos_x = hud_x + PLAYERTEXT_XPOS;
        text->pos_y = PLAYERTEXT_YPOS;
        text->pos_z = PLAYERTEXT_ZPOS;
        text->box_size_x = PLAYERTEXT_WIDTH;
        text->box_size_y = PLAYERTEXT_WIDTH;

        /* Create Player Name Text: Text_InitializeSubtext, Text_UpdateSubtextSize */
        HSD_SisLib_803A6B98(text, 0.0F, 0.0F, name);
        HSD_SisLib_803A7548(text, 0, PLAYERTEXT_SIZE, PLAYERTEXT_SIZE);

        /* Create Text Background */
        bg_gobj = GObj_Create(14, 15, 0);
        bg_jobj = HSD_JObjLoadJoint(msl_present_nametag_joint());
        HSD_GObjObject_80390A70(bg_gobj, HSD_GObj_804D7849, bg_jobj);
        GObj_SetupGXLink(bg_gobj, HSD_GObj_JObjCallback, TEXT_GXLINK, 0);

        hud = ifAll_802F3424(count);
        bg_jobj->translate.x = hud->x + PLAYERBG_XOFST;
        bg_jobj->translate.y = PLAYERBG_YOFST;
        bg_jobj->scale.y = PLAYERBG_YSCALE;
        /* Get JOBJ 1; Z transform = 0 */
        lb_80011E24(bg_jobj, &joint1, 1, -1);
        joint1->translate.z = 0.0F;
        /* Remove unneccessary dobjs (the first two); the third one's material gets the opacity and the color */
        {
            HSD_DObj* dobj = joint1->u.dobj;
            HSD_Material* material;

            dobj->flags |= 1;
            dobj = dobj->next;
            dobj->flags |= 1;
            dobj = dobj->next;
            material = dobj->mobj->mat;
            material->alpha = PLAYERBG_OPA;
            material->diffuse.r = 0;
            material->diffuse.g = 0;
            material->diffuse.b = 0;
            material->diffuse.a = 255;
        }

        /* Get total width of characters used in tag */
        {
            const unsigned char* internal_widths = msl_sislib_internal_widths();
            u8** font = (u8**) HSD_SisLib_804D1124[text->font_idx];
            const u8* external_widths = font[1];

            cur = fn_803A6FEC((u8*) text->sis_buffer, 0, &unused_size) + TEXTHEADER_SIZE;
            for (;;) {
                int c = *cur;
                if (c == 0x0F) {
                    break;
                }
                if (c == 0x20) {
                    width += CHAR_WIDTH_MAX - internal_widths[cur[1] * 2];
                    cur += 2;
                } else if (c == 0x40) {
                    width += CHAR_WIDTH_MAX - external_widths[cur[1] * 2];
                    cur += 2;
                } else {
                    cur += 1; /* 0x0B (null) and anything else */
                }
            }
        }
        if (width < TAG_WIDTH_MIN) {
            width = TAG_WIDTH_MIN;
        }
        if (width > TAG_WIDTH_MAX) {
            width = TAG_WIDTH_MAX;
        }
        /* Multiply width by X to get background size */
        bg_jobj->scale.x = (float) width * PLAYERBG_XSCALEMULT;
    }
}

#endif

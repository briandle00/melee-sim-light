/* Viewer build only (MSL_GX_RECORD): the match scene's state the HUD reads, and the scene's calls that start the HUD.
 *
 * The game keeps a match's rules, timer and per-fighter display flags in lbl_8046B6A0 (gm_16AE.c; the simulator keeps its own
 * projections for gameplay in Runtime/match.c). The HUD files read it through gm_8016AE38 and its getters, which are the decomp's
 * (decomp/melee/gm/gm_16AE.c, vendored), so this fills it as fn_8016DCC0 does at the scene's start and advances the timer as
 * fn_8016CD98 does each frame.
 *
 * The rules a replay's game settings give (mode, time limit) come from the environment, MSL_HUD_RULES="mode=1;time=480" (stock mode,
 * 8:00; the default): the match configuration the simulator takes (MslCoreMatchConfig) does not carry them. A tape carries the replay's timer
 * after its last frame ("RUL1"), which replaces that default.
 *
 * refs/melee/src/melee/gm/{gm_16AE.c::{fn_8016DCC0,fn_8016CD98,gm_8016E934_OnEnter,fn_8016E730,fn_8016B7B4,fn_8016B7F8,fn_8016B784},
 *   gm_1601.c::{gm_80167A64,gm_80167BC8}} */

#if MSL_GX_RECORD

#include <stdlib.h>
#include <string.h>

#include "gm_names.h"

#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_16AE.h>
#include <melee/gm/types.h>
#include <melee/lb/lbbgflash.h>
#include <melee/if/ifall.h>
#include "if/ifcoget.h"
#include <melee/if/ifstatus.h>
#include <melee/if/iftime.h>
#include <melee/pl/player.h>

#include "runtime/wire.h"

static int env_int(const char* name, const char* key, int fallback)
{
    const char* text = getenv(name);
    const char* at;
    size_t length = strlen(key);

    if (text == NULL) {
        return fallback;
    }
    for (at = text; *at != '\0'; ++at) {
        if ((at == text || at[-1] == ';') && strncmp(at, key, length) == 0 && at[length] == '=') {
            return atoi(at + length + 1);
        }
    }
    return fallback;
}

static u32 scene_tick; /* gm_801A4BA8 below */

/* the replay's starting timer (the tape's "RUL1" trailer, replay_drive.c): 0 = none, the default 8:00 */
static int tape_match_seconds;

void msl_present_set_match_seconds(int seconds)
{
    tape_match_seconds = seconds;
}

/* fn_8016DCC0 without its player setup (the simulator has made the players) */
void msl_present_gm_init(const MslCoreMatchConfig* config)
{
    lbl_8046B6A0_t* state = gm_8016AE38();
    StartMeleeRules rules;
    int mode = env_int("MSL_HUD_RULES", "mode", 1);
    int seconds = env_int("MSL_HUD_RULES", "time", tape_match_seconds != 0 ? tape_match_seconds : 480);
    int i;

    memset(&rules, 0, sizeof(rules));
    gm_80167A64(&rules);
    rules.x0_0 = mode;
    if (seconds != 0) {
        rules.x0_6 = 1;
        rules.x10 = (u32) seconds;
    }
    rules.is_teams = config->is_teams;
    rules.x1_7 = config->friendly_fire & 1;
    rules.x30 = config->match_damage_ratio;
    rules.disable_pausing = 0;

    scene_tick = 0;
    memset(state, 0, sizeof(*state));
    state->unk_D = 6;
    state->unk_0 = 0;
    state->x24C8 = rules;
    state->x24C.x5 = rules.x0_0;
    state->x24C.is_teams = rules.is_teams;
    if (rules.x7 != 0) {
        state->unk_B = rules.x7;
    }
    state->x24C.xC = rules.xC;
    if (state->x24C8.x0_6) {
        if (state->x24C8.x0_7) {
            state->unk_2C = rules.x14 != 0 ? rules.x14 - 1 : 0;
            state->x24C8.x10 = (u32) -1;
            state->timer_seconds = rules.x10;
        } else {
            state->timer_seconds = rules.x10 == 0 ? (u32) -1 : rules.x10;
            state->unk_2C = rules.x14 != 0 ? rules.x14 - 1 : 0x3B;
        }
    }
    state->is_singleplayer = 0;
    for (i = 0; i < 6; i++) {
        /* the player setup of fn_8016DCC0 / fn_8016E5C0 (gm_16AE.c): each player's color index (Player_GetUnk45), which the effects with the player's color read
         * (the KO star ft_0D31.c, the guard bubble ftCo_Guard.c) and the HUD; the simulator made the players without it and every color read the first one's */
        if (Player_GetPlayerSlotType(i) != Gm_PKind_NA) {
            Player_SetUnk45(i, fn_80160840(gm_80160854(Player_GetPlayerId(i), Player_GetTeam(i), state->x24C8.is_teams == true, Player_GetPlayerSlotType(i))));
        }
        state->FighterMatchInfo[i].x0 = (u8) Player_GetPlayerCharacter(i);
        state->FighterMatchInfo[i].slot_type = (u8) Player_GetPlayerSlotType(i);
        state->FighterMatchInfo[i].spawn_point = -1;
    }
}

/* gm_801A4BA8 (gm_1A45.c): the scene controller's tick counter, gm_80479D58.unk_0, which gm_801A4D34 sets to 0 when the scene starts and adds 1 to
 * after every logic tick. The standings (gm_8016C5C0 and its siblings) are cached under it and recomputed with gm_80166378 when it changes. */
u32 gm_801A4BA8(void)
{
    return scene_tick;
}

/* fn_8016CD98: the frame count and the timer (its audio omitted) */
void msl_present_gm_frame(void)
{
    lbl_8046B6A0_t* arg0 = gm_8016AE38();
    int i;

    if (scene_tick != (u32) -2) {
        scene_tick++;
    }
    if (arg0->hud_enabled == 0) {
        return;
    }
    if ((s32) arg0->frame_count < -1) {
        arg0->frame_count++;
    }
    if (arg0->x24C8.x0_6) {
        if (++arg0->unk_2C >= 60) {
            arg0->unk_2C = 0;
            if (arg0->x24C8.x0_7) {
                if ((s32) arg0->timer_seconds < -1) {
                    arg0->timer_seconds++;
                }
            } else if (arg0->timer_seconds != 0) {
                arg0->timer_seconds -= 1;
            }
        }
    }
    for (i = 0; i < 4; i++) {
        if (arg0->FighterMatchInfo[i].x1 != 0) {
            arg0->FighterMatchInfo[i].x1--;
        }
    }
}

/* The READY / GO overlay's callbacks (fn_8016B7B4, fn_8016B7F8, fn_8016B784) without what the simulator does by hand in
 * msl_core_match_step_prepare at the same frames (ftLib_800868A4, the stage's start, Pokemon Stadium's screens) and the name tags. */
static void overlay_go_end(void)
{
    gm_8016AE38()->hud_enabled = 1;
}

static void overlay_ready_end(void)
{
    lbl_8046B6A0_t* state = gm_8016AE38();

    if (!state->x24C8.x1_3) {
        ifStatus_802F6EA4(4, -1, -1, 0, NULL, (Event) overlay_go_end);
    } else {
        ifStatus_802F6EA4(8, -1, -1, 0, NULL, (Event) overlay_go_end);
    }
}

static void overlay_begin(int id)
{
    (void) id;
}

static int hud_started;

extern void msl_slippi_show_player_names(void);

/* gm_801A4BD4's un_802FF78C (the game's boot makes the debug text's camera and draw object, link 17; they draw nothing without text, but
 * the camera and DevText_DrawAll write the camera setup and the fog off, twice at the end of every frame), then ifAll_802F390C,
 * then gm_8016E934_OnEnter's three calls */
void msl_present_hud_start(void)
{
    lbl_8046B6A0_t* state = gm_8016AE38();

    (void) un_802FF78C();
    ifAll_802F390C();
    lbBgFlash_80021A18(0xFF); /* fn_8016E730 makes the screen flash's objects right after the HUD */
    hud_started = 1;
    if (state->x24C8.x1_2) {
        ifStatus_802F6EA4(8, -1, -1, 0, (Event) overlay_begin, (Event) overlay_ready_end);
    } else {
        ifStatus_802F6EA4(3, -1, -1, 0, (Event) overlay_begin, (Event) overlay_ready_end);
    }
    ifTime_CreateTimers();
    ifStatus_802F665C(state->x24C8.x0_3);
    msl_slippi_show_player_names(); /* Slippi playback's code at the end of gm_Scene_Vs_OnEnter (0x8016E9B4) */
}

#endif

#if MSL_GX_RECORD
/* what a match's end does for the HUD (ifAll_802F3A64) */
void msl_present_hud_stop(void)
{
    if (hud_started) {
        ifAll_802F3A64();
        hud_started = 0;
    }
}
#endif

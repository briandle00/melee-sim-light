#ifndef MSL_RUNTIME_VIEWER_TRACK_H
#define MSL_RUNTIME_VIEWER_TRACK_H

// A per-replay record of state the recording does not carry, for a replay
// viewer: effect spawns, colour overlays, stored charges, model part
// variants, stage animation starts and background state, and the camera.
// Local tooling only. Everything here reads simulation state and never
// writes it; with the track off every hook is one untaken branch.

#include <stdio.h>

#include <platform.h>
#include <baselib/forward.h>
#include <dolphin/mtx.h>

struct MslCoreMatch;

extern int msl_viewer_track_on;

// Start writing events to `out` (one JSON array per line). NULL stops.
void msl_viewer_track_begin(FILE* out);
void msl_viewer_track_end(void);

// Per-frame state, after each msl_core_match_step.
void msl_viewer_track_frame(struct MslCoreMatch* match);

// Event hooks, called only when msl_viewer_track_on is set.
void msl_viewer_track_fx(HSD_GObj* owner, s32 gfx_id, HSD_JObj* jobj,
                         const Vec3* pos, float a0, float a1);
// The frame a hook fires in (the particle probe's lines).
int msl_viewer_track_hook_frame(void);
// The game's random state now: the simulator's, stepped once for each draw
// the game has made this frame that the simulator leaves out.
u32 msl_viewer_track_state(void);
// MSL_TRACK_NO_STEP (a developer's switch): the track as it was before the
// game's own random states went into it - the simulator's state unstepped,
// each piece's seed in the order the simulator draws.
int msl_viewer_track_plain(void);
// A generator the game makes here and the simulator does not: counted when
// its making draws (runtime/effects.h msl_effect_generator_draws).
void msl_viewer_track_skipped(int bank, int generator_id);
// `n` such draws at once (an effect model's animation's first step).
void msl_viewer_track_skipped_n(int n);
void msl_viewer_track_fxend(HSD_GObj* owner);
// One piece of the spawn just written (runtime/effect_pieces.c): what the
// game's dispatch made for it, as an "fxp" event after its "fx".
struct MslEffectPiece;
void msl_viewer_track_fxp(HSD_GObj* owner, s32 gfx_id,
                          const struct MslEffectPiece* piece);
// A joint-animation particle event (efLib_Cb_DPtcl): bank, generator id,
// and the joint it fires on.
void msl_viewer_track_gen(int bank, int gen_id, HSD_JObj* jobj);
void msl_viewer_track_ganim(HSD_GObj* gobj, int anim_set, int anim_index,
                            float start_frame, float rate);
// A Pokemon Stadium jumbotron screen change (grStadium_801D2528): the screen
// shown and, for a close-up (8), the followed player; -1 otherwise.
void msl_viewer_track_stadscreen(int mode, int player);
// Dream Land's background birds (grOldPupupu_80210D10): the row of the bird
// table chosen, how many fly, and where the first starts.
void msl_viewer_track_bird(int kind, int count, const Vec3* pos);
// A generator the stage code spawns itself (grLib_801C96F8 at `pos`, or
// grLib_801C97DC on `jobj`).
void msl_viewer_track_sgen_as(int bank, int gen_id, const Vec3* pos,
                              HSD_JObj* jobj, const char* mark);
void msl_viewer_track_send(const char* how, HSD_JObj* jobj, int tree);
void msl_viewer_track_sgen(int bank, int gen_id, const Vec3* pos,
                           HSD_JObj* jobj);
// A fighter's switchable texture (eyes, mouth): ftAnim_800704F0 sets slot
// `slot` to texture `index`; ftAnim_80070654 puts every slot back to its
// default, written as slot -1, index 0.
void msl_viewer_track_tex(HSD_GObj* fighter, int slot, float index);

// Put a value the replay recorded back in the simulator's game, where the
// simulator's own came out a few float bits away (a joint's matrix a few
// last bits off: a laser's spawn point): kind 'i' the item in that slot of
// the item list, 'f' a port's fighter, 'n' its follower; field 0 the x
// position, 1 the y (an item's 2 and 3: its velocity's). 0 when it was put,
// -1 when there is no such one.
int msl_viewer_track_resync(struct MslCoreMatch* match, char kind, int lane,
                            int field, float value);

#endif

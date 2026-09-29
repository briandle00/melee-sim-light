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
void msl_viewer_track_fxend(HSD_GObj* owner);
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
void msl_viewer_track_sgen(int bank, int gen_id, const Vec3* pos,
                           HSD_JObj* jobj);
// A fighter's switchable texture (eyes, mouth): ftAnim_800704F0 sets slot
// `slot` to texture `index`; ftAnim_80070654 puts every slot back to its
// default, written as slot -1, index 0.
void msl_viewer_track_tex(HSD_GObj* fighter, int slot, float index);

#endif

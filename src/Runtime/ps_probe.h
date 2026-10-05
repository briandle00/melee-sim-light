#ifndef MSL_RUNTIME_PS_PROBE_H
#define MSL_RUNTIME_PS_PROBE_H

// Viewer track only: the particle reference probe (sysdolphin/baselib/
// ps_probe.c). The decompilation's particle library run beside the
// simulation for the generators the effect recorder makes, every particle
// written to MSL_PARTICLE_PROBE=<file> frame by frame. Off, and none of it
// reached, without that variable.

#include <platform.h>
#include <baselib/forward.h>
#include <dolphin/mtx.h>

// how the game's code made a generator (eflib.c)
enum {
    MSL_PROBE_PLACE,     // efLib_CreateGenerator: at a place
    MSL_PROBE_SRT,       // efLib_CreateGenerator_AddAppSRT: under a transform
    MSL_PROBE_JOINT_RAW, // hsd_8039EFAC: on a joint, as the dispatch calls it
    MSL_PROBE_JOINT,     // efLib_CreateGenerator_Attach: on a joint
    MSL_PROBE_JOINT_SRT, // efLib_CreateGenerator_Attach_AddAppSRT
};

// how a stage's code makes a generator (grlib.c), and ends one
enum {
    MSL_PROBE_STAGE_PLACE, // grLib_801C96F8: at a place, under a transform
    MSL_PROBE_STAGE_ONCE,  // grLib_801C97DC: placed once from a joint
    MSL_PROBE_STAGE_JOINT, // grLib_801C9808, an animation's key: on a joint
};
enum {
    MSL_PROBE_END_KEPT,       // grLib_801C9834: the one the stage kept
    MSL_PROBE_END_KEPT_NOW,   // grLib_801C9874: and its particles at once
    MSL_PROBE_END_JOINTS,     // grLib_801C9854: a joint tree's
    MSL_PROBE_END_JOINT_NOW,  // grLib_801C98A0: a joint's, particles too
    MSL_PROBE_END_JOINTS_NOW, // grLib_801C9908: a joint tree's, particles too
};

int msl_particle_probe_on(void);
// A stage's generator made as the game makes it, from the random state
// `state` (put back after). `counted`: its draw is one the simulator leaves
// out (checked against the track's count each frame).
void msl_particle_probe_stage(int how, s32 id, int bank, const Vec3* pos,
                              HSD_JObj* jobj, u32 state, int counted);
void msl_particle_probe_stage_end(int how, HSD_JObj* jobj);
// A generator a particle key starts on one of the simulation's own joints,
// outside the stage's file (efLib_Cb_DPtcl -> efLib_SpawnParticleEffect).
void msl_particle_probe_key(int bank, s32 id, HSD_JObj* jobj, u32 state);
void msl_particle_probe_begin(void);
void msl_particle_probe_end(void);
// A generator made as the game makes it, from the random state `seed_at`
// (put back after); NULL when the probe is off or the bank has none.
void* msl_particle_probe_generator(s32 gfx_id, HSD_JObj* jobj, int how,
                                   int link_no, u32 seed_at);
// What the dispatch then set on it, and its line in the file.
void msl_particle_probe_placed(void* made, int frame, s32 gfx_id, int how,
                               u32 seed_at, const Vec3* pos, u32 kind_set,
                               const Vec3* translate, const Vec3* rotate,
                               const Vec3* scale);
// An effect model made as the game makes it (efLib_Create and its attach
// variants, by the recorder's `how`), with what the dispatch then set on
// it; NULL when the probe is off or the model is not one it can load.
void* msl_particle_probe_model(s32 gfx_id, const char* how, HSD_GObj* parent,
                               HSD_JObj* joint, const Vec3* translate,
                               const Vec3* rotate, const Vec3* scale,
                               const char* callback, int lifetime,
                               const Vec3* params, HSD_JObj* attach,
                               int scale_flags);
// The model the dispatch set as this one's next (effect->next).
void msl_particle_probe_model_pair(void* model, void* next);
// The joint its callback reads its side from: copied, never kept.
void msl_particle_probe_model_side(void* model, const HSD_JObj* from);
// Its animation's first step (the generators its particle keys start),
// from the random state `seed_at` (put back after).
// `counted`: its draws are ones the simulator leaves out (checked against
// the track's count each frame).
void msl_particle_probe_model_started(void* model, int frame, u32 seed_at,
                                      int counted);
// A joint is being freed: the probe's generators on it stay where they are,
// off it (the game's own hold a reference that keeps the joint; the probe
// takes none).
void msl_particle_probe_joint_gone(HSD_JObj* jobj);
// An owner's effects all ended (efLib_DestroyAll): its joints' generators.
void msl_particle_probe_owner_ended(HSD_GObj* owner, int frame);
// the game's effect pause / resume (hitlag) and expiry flag for an owner's
// models: nothing in gameplay, the probe's models take them
void msl_particle_probe_owner_pause(HSD_GObj* owner, int on);
void msl_particle_probe_owner_flags(HSD_GObj* owner, int expire_flags);
// The frame's update (after its gameplay), and everything written.
// `state`: the random state the game's update starts from; `skipped`: how
// many draws the track stepped the simulator's state by this frame.
void msl_particle_probe_frame(int frame, u32 state, int skipped);

#endif

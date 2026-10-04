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

int msl_particle_probe_on(void);
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
// An owner's effects all ended (efLib_DestroyAll): its joints' generators.
void msl_particle_probe_owner_ended(HSD_GObj* owner, int frame);
// The frame's update (after its gameplay), and everything written.
void msl_particle_probe_frame(int frame);

#endif

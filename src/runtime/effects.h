#ifndef MSL_CORE_RUNTIME_EFFECTS_H
#define MSL_CORE_RUNTIME_EFFECTS_H

#include <platform.h>

#include <dolphin/mtx.h>

#include <baselib/forward.h>
#include <baselib/psstructs.h>

#include "ef/forward.h"
#include "gr/forward.h"

enum {
    // Banks index refs/melee/src/melee/ef/efasync.c::efAsync_DatEntries;
    // Luigi's effLuigiDataTable occupies slot 18.
    // Kirby's copy-ability banks reach id 48 (EfKbFe.dat).
    // refs/melee/src/melee/ef/efasync.c::efAsync_DatEntries
    MSL_CORE_EFFECT_BANK_CAPACITY = 49,
    MSL_CORE_EFFECT_QUEUE_CAPACITY = 256,
    MSL_CORE_EFFECT_COMMON_MODEL_CAPACITY = 0x28,
    MSL_CORE_EFFECT_MODEL_START_CAPACITY = 4,
};

typedef struct MslCoreEffectModelStart {
    s32 generator_ids[MSL_CORE_EFFECT_MODEL_START_CAPACITY];
    u8 count;
} MslCoreEffectModelStart;

typedef struct MslCoreEffectQueueNode {
    struct MslCoreEffectQueueNode* next;
    u8 spawn_kind;
    s32 gfx_id;
    HSD_JObj* jobj;
    Vec3 params;
} MslCoreEffectQueueNode;

typedef struct MslCoreEffectGeneratorBank {
    HSD_Archive* archive;
    int* command_bank;
    HSD_PSCmdList** commands;
    EF_EffectDesc* models;
    s32 count;
    // Version-0x4x command banks index entries by the full generator id: the
    // occupied range [id_base, count) is stored at commands[id - id_base].
    // Version-0 banks have id_base 0.
    s32 id_base;
} MslCoreEffectGeneratorBank;

typedef struct MslCoreEffectData {
    MslCoreEffectGeneratorBank banks[MSL_CORE_EFFECT_BANK_CAPACITY];
#ifdef MSL_CORE_NATIVE
    MslCoreEffectGeneratorBank stage_banks[LAST + 1];
#endif
    MslCoreEffectModelStart
        common_model_start[MSL_CORE_EFFECT_COMMON_MODEL_CAPACITY];
    s8 recording_common_model;
} MslCoreEffectData;

typedef struct MslCoreEffectState {
    MslCoreEffectQueueNode nodes[MSL_CORE_EFFECT_QUEUE_CAPACITY];
    MslCoreEffectQueueNode* free;
} MslCoreEffectState;

// The particle probe's (sysdolphin/baselib/ps_probe.c): a loaded bank, or
// NULL.
const MslCoreEffectGeneratorBank* msl_effect_probe_bank(int bank);
// Whether making this generator draws a random number in the game
// (hsd_8039F05C: one whose command counts at random).
int msl_effect_generator_draws(int bank, int generator_id);
// Viewer track only. An effect model (file * 1000 + its place in the file's
// table) outside the ones the simulator draws for itself (the common file's
// first 0x28): how many random numbers the game draws as its animation
// takes its first step - one for each generator its particle keys start
// there whose command counts at random. Recorded once at startup.
int msl_effect_model_first_draws(int model_id);
// How many models an effect file's table has (0: none the track knows).
int msl_effect_model_count(int bank);
// The running stage's internal id (the particle probe's "stage" line).
int msl_particle_probe_stage_id(void);
// The particle probe's: a joint animation's particle keys go to `callback`
// while it runs its own models; NULL puts the match's own back.
void msl_effect_probe_dptcl(void (*callback)(int, int, int, HSD_JObj*));

void msl_effect_game_data_init(MslCoreEffectData* data);
#ifdef MSL_CORE_NATIVE
// Load immutable map_ptcl commands before GameData seals.
int msl_effect_load_stage_bank(MslCoreEffectData* data,
                               s32 internal_stage_id, HSD_Archive* archive);
#endif
// Installs the match-time joint-animation particle callback.
void msl_effect_install_match_callbacks(void);
void msl_effect_match_init(const MslCoreEffectData* data,
                           MslCoreEffectState* state);
void msl_effect_projection_bind(const MslCoreEffectData* data,
                                MslCoreEffectState* state);
// Checked-out nodes; must be zero after a complete frame.
u32 msl_effect_queue_used(void);
// Print checked-out nodes' spawn kinds and effect ids.
void msl_effect_queue_dump(void);

#endif

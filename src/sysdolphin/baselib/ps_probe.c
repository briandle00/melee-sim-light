// Viewer track only: the particle reference probe.
//
// The headless core runs no particles: a generator's creation only draws the
// random number the game's would (runtime/effects.c). A replay viewer that
// wants its particles to be the game's own needs something to compare
// against, so the decompilation's particle library (particle.c, generator.c,
// psappsrt.c) is compiled here once more, as a copy of its own
// (probe_ps/*.c.inc, every global renamed pp_* by probe_ps/names.h, so the
// simulator's stand-ins keep the game's names and gameplay calls none of
// this). With MSL_PARTICLE_PROBE=<file> set beside a viewer track:
//
//   - each generator piece the effect recorder makes (runtime/
//     effect_pieces.c) is made here for real, from the random state the game
//     made it with, on the joint or at the place the dispatch gave it;
//   - at the end of each frame the library's own update runs
//     (efLib_particles_proc_main: every particle, then every generator),
//     from the random state the frame's gameplay left;
//   - every generator and particle is written to the file, frame by frame.
//
// The run is invisible to the simulation: the random seed is put back after
// each use, joints are read and never written (no reference is taken on
// them), and nothing here is reachable without the environment variable.
//
// The file is lines of JSON:
//   ["bank", bank, count, scripts]             a bank's commands, as first used
//   ["miss", id, how]                          a generator the bank has none for
//   ["g", frame, uid, id, linkNo, seed, how, type, kind, x, y, z,
//    srt (0 or 1), tx, ty, tz, rx, ry, rz, sx, sy, sz, count]
//                                              a generator as it was made
//   ["f", frame, seed]                         the state the update starts from
//   ["u", frame, uid, type, kind, x, y, z, srt, tx, ty, tz, rx, ry, rz,
//    sx, sy, sz, turned (0 or 1), m00 .. m22]  a generator before the update,
//                                              gone along its joint; the turn
//                                              its emission takes from it
//   ["e", frame, uid]                          a generator ended by its owner
//   ["x", frame]                               an owner's effects ended (the
//                                              list was walked)
//   ["p", frame, list, index, uid, kind, life, x, y, z, vx, vy, vz, size,
//    rotate, prim, env, pose, cmdPtr, cmdWait, grav, fric, srt uid]
//                                              a particle after the update, in
//                                              its list's order (newest first)
//
// refs/melee/src/sysdolphin/baselib/{particle.c,generator.c,psappsrt.c},
// refs/melee/src/melee/ef/eflib.c

#include "probe_ps/names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/object.h>
#include <sysdolphin/baselib/random.h>

#include "probe_ps/psstructs.h"
#include "probe_ps/particle.h"
#include "probe_ps/psappsrt.h"
#include "probe_ps/generator.h"

// (joints are the simulation's: the library's own references on them are
// left out, and a generator is ended with its owner before its joint goes)
static inline void pp_no_ref(void* o) { (void) o; }
#define ref_INC(o) pp_no_ref(o)
#define HSD_JObjUnref(o) pp_no_ref(o)

// (and so are its object heaps, which are saved and restored with it: the
// probe's particles, generators and transforms come from the C heap)
enum { PP_HEAPS = 8 };
static struct {
    const void* data;
    size_t size;
} pp_heaps[PP_HEAPS];
static void pp_alloc_init(void* data, u32 size, u32 align)
{
    int i;
    (void) align;
    for (i = 0; i < PP_HEAPS; ++i) {
        if (pp_heaps[i].data == data || pp_heaps[i].data == NULL) {
            pp_heaps[i].data = data;
            pp_heaps[i].size = size;
            return;
        }
    }
}
static void* pp_alloc(void* data)
{
    int i;
    for (i = 0; i < PP_HEAPS; ++i) {
        if (pp_heaps[i].data == data) {
            return malloc(pp_heaps[i].size);
        }
    }
    return NULL;
}
static void probe_forget(const void* obj);
static void pp_free(void* data, void* obj)
{
    (void) data;
    probe_forget(obj);
    free(obj);
}
#define HSD_ObjAllocInit(d, s, a) pp_alloc_init(d, s, a)
#define HSD_ObjAlloc(d) pp_alloc(d)
#define HSD_ObjFree(d, o) pp_free(d, o)

#include "probe_ps/psappsrt.c.inc"
#include "probe_ps/particle.c.inc"
#include "probe_ps/generator.c.inc"

#undef ref_INC
#undef HSD_ObjAllocInit
#undef HSD_ObjAlloc
#undef HSD_ObjFree
#undef HSD_JObjUnref

#include "runtime/effects.h"
#include "runtime/ps_probe.h"
#include "platform/native_dat.h"

static FILE* probe_out;
static int probe_frame_no;

enum { PROBE_BANKS = 65, PROBE_GENS = 4096 };
static HSD_PSCmdList** probe_commands[PROBE_BANKS];
static const void* probe_bank_source[PROBE_BANKS];
// (no textures here: every texture group reads as absent)
static HSD_PSTexGroup* probe_no_textures[256];

// each live generator's number in the file (its address is reused)
static struct {
    HSD_Generator* gen;
    int uid;
} probe_gens[PROBE_GENS];
static int probe_gen_count;
static int probe_next_uid;

#ifdef _WIN32
// a crash with the probe on says where (the frames' addresses, less the
// image's base: addr2line -e replay_drive.exe <address + 0x140000000>)
#undef seed
#include <windows.h>
static LONG WINAPI probe_crashed(EXCEPTION_POINTERS* info)
{
    void* frames[24];
    USHORT n;
    USHORT i;
    char* base = (char*) GetModuleHandleA(NULL);
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fprintf(stderr, "probe: access violation at +0x%llx (address 0x%llx), frame %d\n",
            (unsigned long long) ((char*) info->ExceptionRecord->ExceptionAddress - base),
            (unsigned long long) info->ExceptionRecord->ExceptionInformation[1], probe_frame_no);
    n = CaptureStackBackTrace(0, 24, frames, NULL);
    for (i = 0; i < n; ++i) {
        fprintf(stderr, " +0x%llx", (unsigned long long) ((char*) frames[i] - base));
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    if (probe_out != NULL) {
        fflush(probe_out);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int msl_particle_probe_on(void)
{
    return probe_out != NULL;
}

void msl_particle_probe_begin(void)
{
    const char* path = getenv("MSL_PARTICLE_PROBE");
    int i;

    if (probe_out != NULL) {
        fclose(probe_out);
        probe_out = NULL;
    }
    if (path == NULL || path[0] == '\0') {
        return;
    }
    probe_out = fopen(path, "wb");
    if (probe_out == NULL) {
        return;
    }
#ifdef _WIN32
    AddVectoredExceptionHandler(1, probe_crashed);
#endif
    // efLib_Init's
    hsd_8039D354(0);
    hsd_80398A08(0);
    psInitAppSRT(0, sizeof(HSD_psAppSRT));
    for (i = 0; i < PROBE_BANKS; ++i) {
        probe_commands[i] = NULL;
        probe_bank_source[i] = NULL;
    }
    probe_gen_count = 0;
    probe_next_uid = 0;
    probe_frame_no = 0;
}

void msl_particle_probe_end(void)
{
    if (probe_out != NULL) {
        fclose(probe_out);
        probe_out = NULL;
    }
}

// A bank as the library reads one: the simulator's translated commands,
// each with its script (the bytes after its header in the archive).
static int probe_bank(int bank)
{
    const MslCoreEffectGeneratorBank* data = msl_effect_probe_bank(bank);
    int i;
    int scripts = 0;

    if (bank < 0 || bank >= PROBE_BANKS || data == NULL ||
        data->commands == NULL || data->count <= 0)
    {
        return 0;
    }
    if (probe_commands[bank] != NULL &&
        probe_bank_source[bank] == (const void*) data->commands)
    {
        return 1;
    }
    probe_commands[bank] = calloc((size_t) data->count, sizeof(HSD_PSCmdList*));
    probe_bank_source[bank] = data->commands;
    for (i = 0; i < data->count; ++i) {
        const HSD_PSCmdList* command = data->commands[i];
        uint32_t size = 0;
        const uint8_t* script;
        HSD_PSCmdList* copy;

        if (command == NULL) {
            continue;
        }
        script = msl_native_particle_script(command, &size);
        if (script == NULL) {
            continue;
        }
        copy = calloc(1, offsetof(HSD_PSCmdList, cmdList) + size + 8);
        memcpy(copy, command, offsetof(HSD_PSCmdList, cmdList));
        // (psInitDataBankLocate's: every command's kind is given these bits)
        copy->kind = (copy->kind & 0xF1FFFFFF) | 0x08000000;
        memcpy(copy->cmdList, script, size);
        // (past its end, should a script run off it: the end command)
        memset(copy->cmdList + size, 0xFF, 8);
        probe_commands[bank][i] = copy;
        scripts++;
    }
    ptclref_804D0E5C[bank] = probe_commands[bank];
    psCmdListArray[bank] = data->count;
    psTexGroupArray[bank] = probe_no_textures;
    psNumCmdList[bank] = NULL;
    hsd_804D0948[bank] = NULL;
    fprintf(probe_out, "[\"bank\",%d,%d,%d]\n", bank, (int) data->count, scripts);
    return 1;
}

// (a freed generator's number goes with it: its address is given out again,
// to a particle's own generator within the same update)
static void probe_forget(const void* obj)
{
    int i;
    for (i = 0; i < probe_gen_count; ++i) {
        if ((const void*) probe_gens[i].gen == obj) {
            probe_gens[i] = probe_gens[--probe_gen_count];
            return;
        }
    }
}

static int probe_uid(const HSD_Generator* gen)
{
    int i;
    for (i = 0; i < probe_gen_count; ++i) {
        if (probe_gens[i].gen == gen) {
            return probe_gens[i].uid;
        }
    }
    return -1;
}

static int probe_live(const HSD_Generator* gen)
{
    const HSD_Generator* g;
    for (g = hsd_804D78FC; g != NULL; g = g->next) {
        if (g == gen) {
            return 1;
        }
    }
    return 0;
}

// the numbers of the generators that are gone are let go
static void probe_sweep(void)
{
    int i;
    int kept = 0;
    for (i = 0; i < probe_gen_count; ++i) {
        if (probe_live(probe_gens[i].gen)) {
            probe_gens[kept++] = probe_gens[i];
        }
    }
    probe_gen_count = kept;
}

void* msl_particle_probe_generator(s32 gfx_id, HSD_JObj* jobj, int how,
                                   int link_no, u32 seed_at)
{
    HSD_Generator* gen;
    u32 saved;
    int bank = gfx_id / 1000;

    if (probe_out == NULL || !probe_bank(bank)) {
        return NULL;
    }
    saved = *seed_ptr;
    *seed_ptr = seed_at;
    switch (how) {
    case MSL_PROBE_PLACE:
        // efLib_CreateGenerator
        switch (gfx_id) {
        case 0x121:
            link_no = 2;
            break;
        case 0xFF:
        case 0x7918:
        case 0xFC:
        case 0xF7:
            link_no = 1;
            break;
        default:
            link_no = 0;
            break;
        }
        gen = hsd_8039F05C(link_no, bank, gfx_id);
        break;
    case MSL_PROBE_SRT:
        // efLib_CreateGenerator_AddAppSRT
        gen = hsd_8039F05C(0, bank, gfx_id);
        if (gen != NULL && gen->appsrt == NULL &&
            psAddGeneratorAppSRT_begin(gen, 1) == NULL)
        {
            hsd_8039D4DC(gen);
            gen = NULL;
        }
        break;
    case MSL_PROBE_JOINT_RAW:
        // the dispatch's own hsd_8039EFAC
        gen = hsd_8039EFAC(link_no, bank, gfx_id, jobj);
        break;
    case MSL_PROBE_JOINT:
        // efLib_CreateGenerator_Attach
        gen = hsd_8039EFAC(0, bank, gfx_id, jobj);
        if (gen != NULL) {
            gen->type &= ~0x400;
        }
        break;
    case MSL_PROBE_JOINT_SRT:
        // efLib_CreateGenerator_Attach_AddAppSRT
        gen = hsd_8039EFAC(0, bank, gfx_id, jobj);
        if (gen != NULL) {
            HSD_psAppSRT* srt = gen->appsrt;
            if (srt == NULL) {
                srt = psAddGeneratorAppSRT_begin(gen, 0);
            }
            if (srt != NULL) {
                srt->gp = gen;
                gen->type &= 0xFFFFF9FF;
                gen->type |= 0x800;
            } else {
                hsd_8039D4DC(gen);
                gen = NULL;
            }
        }
        break;
    default:
        gen = NULL;
        break;
    }
    *seed_ptr = saved;
    if (gen == NULL) {
        fprintf(probe_out, "[\"miss\",%d,%d]\n", (int) gfx_id, how);
    }
    if (gen != NULL && probe_gen_count < PROBE_GENS) {
        probe_gens[probe_gen_count].gen = gen;
        probe_gens[probe_gen_count].uid = probe_next_uid++;
        probe_gen_count++;
    }
    return gen;
}

static void probe_srt(const HSD_psAppSRT* srt)
{
    if (srt == NULL) {
        fprintf(probe_out, ",0,0,0,0,0,0,0,1,1,1");
        return;
    }
    fprintf(probe_out, ",1,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
            srt->translate.x, srt->translate.y, srt->translate.z, srt->rot.x,
            srt->rot.y, srt->rot.z, srt->scale.x, srt->scale.y, srt->scale.z);
}

void msl_particle_probe_placed(void* made, int frame, s32 gfx_id, int how,
                               u32 seed_at, const Vec3* pos, u32 kind_set,
                               const Vec3* translate, const Vec3* rotate,
                               const Vec3* scale)
{
    HSD_Generator* gen = made;

    if (probe_out == NULL || gen == NULL || !probe_live(gen)) {
        return;
    }
    if (how == MSL_PROBE_PLACE) {
        gen->pos = *pos;
    }
    gen->kind |= kind_set;
    if (gen->appsrt != NULL && (how == MSL_PROBE_SRT || how == MSL_PROBE_JOINT_SRT)) {
        gen->appsrt->translate = *translate;
        gen->appsrt->rot.x = rotate->x;
        gen->appsrt->rot.y = rotate->y;
        gen->appsrt->rot.z = rotate->z;
        gen->appsrt->scale = *scale;
    }
    fprintf(probe_out, "[\"g\",%d,%d,%d,%d,%u,%d,%u,%u,%.9g,%.9g,%.9g", frame,
            probe_uid(gen), (int) gfx_id, (int) gen->linkNo,
            (unsigned) seed_at, how, (unsigned) gen->type,
            (unsigned) gen->kind, gen->pos.x, gen->pos.y, gen->pos.z);
    probe_srt(gen->appsrt);
    fprintf(probe_out, ",%.9g]\n", gen->count);
}

// hsd_8039D688 over a joint tree (efLib_DestroyAll's HSD_JObjWalkTree)
static void probe_end_tree(HSD_JObj* jobj, int frame)
{
    while (jobj != NULL) {
        HSD_Generator* gen;
        for (gen = hsd_804D78FC; gen != NULL; gen = gen->next) {
            if (gen->jobj == jobj) {
                fprintf(probe_out, "[\"e\",%d,%d]\n", frame, probe_uid(gen));
            }
        }
        hsd_8039D688(jobj, NULL, 0);
        // (one that lives on for its particles' sake: its joint is not ours
        // to keep, and it follows it no more)
        for (gen = hsd_804D78FC; gen != NULL; gen = gen->next) {
            if (gen->jobj == jobj) {
                gen->jobj = NULL;
            }
        }
        if (!(jobj->flags & JOBJ_INSTANCE)) {
            probe_end_tree(jobj->child, frame);
        }
        jobj = jobj->next;
    }
}

void msl_particle_probe_owner_ended(HSD_GObj* owner, int frame)
{
    if (probe_out == NULL || owner == NULL || owner->hsd_obj == NULL) {
        return;
    }
    {
        HSD_JObj* root = owner->hsd_obj;
        HSD_JObj* next = root->next;
        // (the owner's own tree only, not its siblings)
        root->next = NULL;
        probe_end_tree(root, frame);
        root->next = next;
    }
    // (the walk itself: where the next new generator goes depends on it)
    fprintf(probe_out, "[\"x\",%d]\n", frame);
    probe_sweep();
}

static unsigned probe_rgba(GXColor c)
{
    return ((unsigned) c.r << 24) | ((unsigned) c.g << 16) |
           ((unsigned) c.b << 8) | (unsigned) c.a;
}

void msl_particle_probe_frame(int frame)
{
    HSD_Generator* gen;
    u32 saved;
    int list;

    if (probe_out == NULL) {
        return;
    }
    probe_frame_no = frame;
    saved = *seed_ptr;
    fprintf(probe_out, "[\"f\",%d,%u]\n", frame, (unsigned) saved);
    // each generator gone along its joint, as the update is about to take it
    for (gen = hsd_804D78FC; gen != NULL; gen = gen->next) {
        if (!(gen->kind & 0x800)) {
            hsd_8039D214(gen);
        }
        fprintf(probe_out, "[\"u\",%d,%d,%u,%u,%.9g,%.9g,%.9g", frame,
                probe_uid(gen), (unsigned) gen->type, (unsigned) gen->kind,
                gen->pos.x, gen->pos.y, gen->pos.z);
        probe_srt(gen->appsrt);
        // (the turn its emission takes from its joint, as hsd_8039DAD4 reads it)
        if ((gen->type & 0x100) && gen->jobj != NULL && (gen->type & 0x400) &&
            !(gen->kind & 0x30000))
        {
            int c;
            float m[3][3];
            for (c = 0; c < 3; ++c) {
                Vec3 v;
                v.x = gen->jobj->mtx[0][c];
                v.y = gen->jobj->mtx[1][c];
                v.z = gen->jobj->mtx[2][c];
                PSVECNormalize(&v, &v);
                m[0][c] = v.x;
                m[1][c] = v.y;
                m[2][c] = v.z;
            }
            fprintf(probe_out, ",1,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                    m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2],
                    m[2][0], m[2][1], m[2][2]);
        } else {
            fprintf(probe_out, ",0");
        }
        fprintf(probe_out, "]\n");
    }
    // efLib_particles_proc_main, then _aux: all sixteen lists between them
    hsd_8039CEAC(0x20000 | 0x40000);
    hsd_8039EE24(0x20000 | 0x40000);
    hsd_8039CEAC(0x10000);
    hsd_8039EE24(0x10000);
    *seed_ptr = saved;
    for (list = 0; list < 16; ++list) {
        const HSD_Particle* pp;
        int index = 0;
        for (pp = hsd_804D0908[list]; pp != NULL; pp = pp->next, ++index) {
            fprintf(probe_out,
                    "[\"p\",%d,%d,%d,%d,%u,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                    "%.9g,%.9g,%u,%u,%d,%d,%d,%.9g,%.9g,%d]\n",
                    frame, list, index, probe_uid(pp->gen),
                    (unsigned) pp->kind, (int) pp->life, pp->pos.x, pp->pos.y,
                    pp->pos.z, pp->vel.x, pp->vel.y, pp->vel.z, pp->size,
                    pp->rotate, probe_rgba(pp->primCol),
                    probe_rgba(pp->envCol), (int) pp->poseNum,
                    (int) pp->cmdPtr, (int) pp->cmdWait, pp->grav, pp->fric,
                    pp->appsrt != NULL && pp->appsrt->gp != NULL
                        ? probe_uid(pp->appsrt->gp)
                        : (pp->appsrt != NULL ? -2 : -1));
        }
    }
    probe_sweep();
}

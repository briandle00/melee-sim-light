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
//   ["m", frame, model]                        an effect model made (its
//                                              animation's generators follow
//                                              as "g" lines, how 5)
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
//   ["stage", id]                              the stage (its internal id)
//   ["s", frame, uid, x, y, z, vx, vy, vz, ax, ay, az]
//                                              a generator placed once from
//                                              its joint (hsd_8039D71C): its
//                                              place, motion and line, turned
//   ["k", frame, skipped, drawn]               the draws the track stepped the
//                                              state by, and the draws the
//                                              probe's real makings made
//   ["j", frame, uid]                          a generator taken off its joint
//                                              (the game freed the joint)
//   ["e", frame, uid, mode]                    mode 1: ended (hsd_8039D4DC),
//                                              2: and its particles at once
//   ["e", frame, uid]                          a generator ended by its owner
//   ["x", frame]                               an owner's effects ended (the
//                                              list was walked)
//   ["a", frame, uid, command, count, genLife, particles, type, kind]
//                                              a generator after the update,
//                                              in the list's order
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

#include "runtime/relocation.h"

// (joints are the simulation's: the library's own references on them are
// left out, and a generator is ended with its owner before its joint goes.
// The joints the library makes for itself - one for a leading particle, at
// its place, which the particles after it are drawn to - are the probe's
// own: a matrix whose place is set directly, none of the simulation's heap)
enum { PP_LEADERS = 16 };
static HSD_JObj* pp_leaders[PP_LEADERS];
static int pp_is_leader(const void* o)
{
    int i;
    for (i = 0; i < PP_LEADERS; ++i) {
        if (o != NULL && pp_leaders[i] == o) {
            return 1;
        }
    }
    return 0;
}
static HSD_JObj* pp_leader_new(void)
{
    int i;
    for (i = 0; i < PP_LEADERS; ++i) {
        if (pp_leaders[i] == NULL) {
            pp_leaders[i] = calloc(1, sizeof(HSD_JObj));
            return pp_leaders[i];
        }
    }
    return NULL;
}
static inline void pp_ref(void* o) { (void) o; }
static void pp_unref(void* o)
{
    int i;
    for (i = 0; i < PP_LEADERS; ++i) {
        if (o != NULL && pp_leaders[i] == o) {
            // (made with one reference and given one more by its slot: gone
            // when the slot lets it go, the second time it is let go)
            if (pp_leaders[i]->translate.x != 0.0F) {
                free(pp_leaders[i]);
                pp_leaders[i] = NULL;
            } else {
                pp_leaders[i]->translate.x = 1.0F;
            }
            return;
        }
    }
}
static int pm_owns(const HSD_JObj* jobj);
static void pp_setup_matrix(HSD_JObj* jobj)
{
    if (pp_is_leader(jobj)) {
        return;
    }
    // (a probe model's joint: what its setup makes on first use is the
    // probe's too)
    if (pm_owns(jobj)) {
        int was = msl_probe_heap;
        msl_probe_heap = 1;
        HSD_JObjSetupMatrix(jobj);
        msl_probe_heap = was;
    } else {
        HSD_JObjSetupMatrix(jobj);
    }
}
static void pp_add_place(HSD_JObj* jobj, int row, float by)
{
    jobj->mtx[row][3] += by;
}
// (the list of generators waiting to be placed from their joints: its nodes
// the probe's own, put in as the game's list functions put them)
static HSD_SList* pp_slist_append(HSD_SList* list, void* data)
{
    HSD_SList* node = calloc(1, sizeof(HSD_SList));
    node->data = data;
    if (list != NULL) {
        node->next = list->next;
        list->next = node;
        return list;
    }
    return node;
}
static HSD_SList* pp_slist_remove(HSD_SList* list)
{
    HSD_SList* next;
    if (list == NULL) {
        return NULL;
    }
    next = list->next;
    free(list);
    return next;
}
#define HSD_SListAllocAndAppend(l, d) pp_slist_append(l, d)
#define HSD_SListRemove(l) pp_slist_remove(l)
#define ref_INC(o) pp_ref(o)
#define HSD_JObjUnref(o) pp_unref(o)
#define HSD_JObjAlloc() pp_leader_new()
#define HSD_JObjSetupMatrix(o) pp_setup_matrix(o)
#define HSD_JObjAddTranslationX(o, by) pp_add_place(o, 0, by)
#define HSD_JObjAddTranslationY(o, by) pp_add_place(o, 1, by)
#define HSD_JObjAddTranslationZ(o, by) pp_add_place(o, 2, by)

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
#undef HSD_SListAllocAndAppend
#undef HSD_SListRemove
#undef HSD_JObjAlloc
#undef HSD_JObjSetupMatrix
#undef HSD_JObjAddTranslationX
#undef HSD_JObjAddTranslationY
#undef HSD_JObjAddTranslationZ
#undef HSD_ObjAllocInit
#undef HSD_ObjAlloc
#undef HSD_ObjFree
#undef HSD_JObjUnref

#include "runtime/effects.h"
#include "runtime/ps_probe.h"
#include "runtime/relocation.h"
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

static void probe_made(HSD_Generator* gen);
static int probe_drawn;
static u32 saved_real;
static HSD_Generator* probe_stage_kept;
static int probe_stage_told;
static void pm_frame(int frame);
static void pm_owner_ended(HSD_GObj* owner, int frame);
static void pm_reset(void);

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
    hsd_804D7900 = probe_made;
    for (i = 0; i < PROBE_BANKS; ++i) {
        probe_commands[i] = NULL;
        probe_bank_source[i] = NULL;
    }
    pm_reset();
    probe_stage_kept = NULL;
    probe_stage_told = 0;
    probe_drawn = 0;
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

// every generator made gets a number: the ones made here and the ones a
// particle's script makes (the library's own hook, run as one is made)
// (the ones an effect model's animation starts are written once its step
// is done: collected here as they are made)
static HSD_Generator* pm_pending[64];
static int pm_pending_count;
static int pm_collect;

static void probe_made(HSD_Generator* gen)
{
    if (pm_collect && pm_pending_count < 64) {
        pm_pending[pm_pending_count++] = gen;
    }
    if (probe_gen_count < PROBE_GENS) {
        probe_gens[probe_gen_count].gen = gen;
        probe_gens[probe_gen_count].uid = probe_next_uid++;
        probe_gen_count++;
    }
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
    pm_owner_ended(owner, frame);
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

void msl_particle_probe_frame(int frame, u32 state, int skipped)
{
    HSD_Generator* gen;
    u32 saved;
    int list;

    if (probe_out == NULL) {
        return;
    }
    probe_frame_no = frame;
    if (skipped != 0 || probe_drawn != 0) {
        fprintf(probe_out, "[\"k\",%d,%d,%d]\n", frame, skipped, probe_drawn);
    }
    probe_drawn = 0;
    // (the models first: their generators go along the joints as they are
    // now, and what their animations' later keys draw comes before the
    // particles' own draws - the update goes on from where they leave it)
    saved_real = *seed_ptr;
    *seed_ptr = state;
    pm_frame(frame);
    state = *seed_ptr;
    *seed_ptr = saved_real;
    // (the ones waiting to be placed from their joints, as the update's
    // first lines place them: hsd_8039EE24)
    while (hsd_804D78F4 != 0) {
        HSD_Generator* gp = ((HSD_SList*) hsd_804D78F4)->data;
        hsd_8039D71C(gp);
        hsd_804D78F4 = (uintptr_t) pp_slist_remove((HSD_SList*) hsd_804D78F4);
        gp->jobj = NULL;
        fprintf(probe_out, "[\"s\",%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g]\n",
                frame, probe_uid(gp), gp->pos.x, gp->pos.y, gp->pos.z,
                gp->vel.x, gp->vel.y, gp->vel.z, gp->aux.line.x2,
                gp->aux.line.y2, gp->aux.line.z2);
    }
    saved = *seed_ptr;
    *seed_ptr = state;
    saved_real = saved;
    saved = state;
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
    *seed_ptr = saved_real;
    for (gen = hsd_804D78FC; gen != NULL; gen = gen->next) {
        // (which command it is of: the one whose script it runs)
        int idx = -1;
        int i;
        const MslCoreEffectGeneratorBank* data = msl_effect_probe_bank(gen->bank);
        for (i = 0; data != NULL && probe_commands[gen->bank] != NULL && i < data->count; ++i) {
            if (probe_commands[gen->bank][i] != NULL &&
                probe_commands[gen->bank][i]->cmdList == gen->cmdList)
            {
                idx = i;
                break;
            }
        }
        fprintf(probe_out, "[\"a\",%d,%d,%d,%.9g,%d,%d,%u,%u]\n", frame,
                probe_uid(gen), idx, gen->count, (int) gen->genLife,
                (int) gen->numChild, (unsigned) gen->type, (unsigned) gen->kind);
    }
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

// --- the effect models ---------------------------------------------------------
//
// A model piece's own generators: its animation's particle keys start them
// (efLib_Cb_DPtcl -> efLib_SpawnParticleEffect) on its joints, on the first
// step after the spawn and, for a few, later. The decompilation's eflib.c
// model half is compiled here as a copy of its own (probe_ps/eflib.c.inc,
// pm_* by probe_ps/eflib_names.h): efLib_Create and its attach variants,
// efLib_Update, the update callbacks, efLib_SpawnParticleEffect. The model is
// loaded for real (the simulator's joints, animations and constraints, from
// its object heaps); the game object it hangs on is a stand-in of the
// probe's, and its per-frame call is made here.
//
// refs/melee/src/melee/ef/eflib.c

#include "probe_ps/eflib_names.h"

#include <math.h>

#include "ef/efasync.h"
#include "ef/eflib.h"
#include "ef/types.h"
#include "ft/fighter.h"
#include "ft/inlines.h"
#include "ft/types.h"
#include "ftCommon/ftCo_Bury.h"
#include "lb/lb_00B0.h"
#include "lb/lbspdisplay.h"
#include <baselib/gobj.h>

enum { PM_MODELS = 64 };
static struct {
    HSD_GObj* gobj;
    int dead;
    int number;
} pm_models[PM_MODELS];
static int pm_next_number;

static HSD_ObjAllocData pm_alloc_data;
static int pm_effect_count;
static volatile u32 pm_load_kind;
static volatile s32 pm_anim_count;
static HSD_JObj* pm_anim_queue[32];
static EF_ParamEntry pm_param_table[8];
static EF_DAT_Entry pm_dat_entries[51];

static int pm_owns(const HSD_JObj* jobj)
{
    int i;
    if (jobj == NULL) {
        return 0;
    }
    while (jobj->parent != NULL) {
        jobj = jobj->parent;
    }
    for (i = 0; i < PM_MODELS; ++i) {
        if (pm_models[i].gobj != NULL && pm_models[i].gobj->hsd_obj == (void*) jobj) {
            return 1;
        }
    }
    return 0;
}

static HSD_GObj* pm_gobj_new(void)
{
    return calloc(1, sizeof(HSD_GObj));
}

static void pm_live(HSD_GObj* gobj)
{
    int i;
    for (i = 0; i < PM_MODELS; ++i) {
        if (pm_models[i].gobj == NULL) {
            pm_models[i].gobj = gobj;
            pm_models[i].dead = 0;
            pm_models[i].number = pm_next_number++;
            return;
        }
    }
}

// (its joints go once no generator is on one of them: the game's own keep
// theirs alive by a reference, which the probe does not take)
static void pm_gobj_free(HSD_GObj* gobj)
{
    int i;
    for (i = 0; i < PM_MODELS; ++i) {
        if (pm_models[i].gobj == gobj) {
            pm_models[i].dead = 1;
        }
    }
}

#define efAsync_DatEntries pm_dat_entries
#define efLib_AllocData pm_alloc_data
#define efLib_EffectCount pm_effect_count
#define efLib_LoadKind pm_load_kind
#define efLib_AnimCount pm_anim_count
#define efLib_AnimQueue pm_anim_queue
#define efLib_ParamTable pm_param_table
#define efLib_RemoveLast() ((void) 0)
#define HSD_GObj_JObjKind HSD_GObj_804D7849
#define GET_EFFECT(gobj) ((EF_Effect*) (gobj)->user_data)
#define GObj_Create(a, b, c) pm_gobj_new()
#define GObj_SetupGXLink(a, b, c, d) ((void) 0)
#define GObj_InitUserData(g, k, f, d) ((g)->user_data = (d))
#define HSD_GObjObject_80390A70(g, k, j) ((g)->obj_kind = (k), (g)->hsd_obj = (j))
#define HSD_GObj_SetupProc(g, f, p) pm_live(g)
#define HSD_GObjFree(g) pm_gobj_free(g)
#define HSD_ObjAlloc(d) pp_alloc(d)
#define HSD_ObjFree(d, o) pp_free(d, o)

#include "probe_ps/eflib.c.inc"

#undef HSD_GObjFree
#undef HSD_ObjAlloc
#undef HSD_ObjFree

static void pm_dptcl(int link_no, int bank, int gfx_id, HSD_JObj* jobj)
{
    (void) link_no;
    // (a stage's are the stage's own: grLib_801C99C0)
    if (bank == 0x1E || !probe_bank(bank)) {
        return;
    }
    efLib_SpawnParticleEffect(bank, gfx_id, jobj, 1);
}

static int probe_command_of(const HSD_Generator* gen)
{
    const MslCoreEffectGeneratorBank* data = msl_effect_probe_bank(gen->bank);
    int i;
    for (i = 0; data != NULL && probe_commands[gen->bank] != NULL && i < data->count; ++i) {
        if (probe_commands[gen->bank][i] != NULL &&
            probe_commands[gen->bank][i]->cmdList == gen->cmdList)
        {
            return i;
        }
    }
    return -1;
}

// the generators an animation step made, written in the order they were made
static void pm_written(int frame)
{
    int i;
    for (i = 0; i < pm_pending_count; ++i) {
        HSD_Generator* gen = pm_pending[i];
        if (!probe_live(gen)) {
            continue;
        }
        fprintf(probe_out, "[\"g\",%d,%d,%d,%d,0,5,%u,%u,%.9g,%.9g,%.9g", frame,
                probe_uid(gen), probe_command_of(gen), (int) gen->linkNo,
                (unsigned) gen->type, (unsigned) gen->kind, gen->pos.x,
                gen->pos.y, gen->pos.z);
        probe_srt(gen->appsrt);
        fprintf(probe_out, ",%.9g]\n", gen->count);
    }
    pm_pending_count = 0;
}

// one animation step of a model, its particle keys the probe's
static void pm_step(HSD_GObj* gobj, int frame, int first)
{
    pm_collect = 1;
    pm_pending_count = 0;
    msl_probe_heap = 1;
    msl_effect_probe_dptcl(pm_dptcl);
    if (first) {
        HSD_JObjAnimAll(gobj->hsd_obj);
    } else {
        efLib_Update(gobj);
    }
    msl_effect_probe_dptcl(NULL);
    msl_probe_heap = 0;
    pm_collect = 0;
    pm_written(frame);
}

static const struct {
    const char* name;
    EF_UpdateFn fn;
} pm_callbacks[] = {
#define PM_CALLBACK(name) { #name, (EF_UpdateFn) efLib_Cb_##name },
    PM_CALLBACK(SetOffsetY_FromParamY) PM_CALLBACK(SetScale_FromParamX)
    PM_CALLBACK(SetRotYAndTransition) PM_CALLBACK(SetJObjOffsetZ)
    PM_CALLBACK(SetRotY_FromFighterDir) PM_CALLBACK(SetRotYZ_FromFighter)
    PM_CALLBACK(Fall_FromParamY) PM_CALLBACK(SetOffset_FromParams)
    PM_CALLBACK(LifetimeEndSpawn) PM_CALLBACK(SetScaleRotY_FromFighter)
    PM_CALLBACK(SetRotYZ_FromParamZ_FighterDir) PM_CALLBACK(ftMr_SpecialLw)
    PM_CALLBACK(ftLg_SpecialLw) PM_CALLBACK(ftKp_SpecialHi)
    PM_CALLBACK(ftCo_Bury) PM_CALLBACK(ApplyStoredAlpha)
    PM_CALLBACK(AccumOffset_FromParams)
#undef PM_CALLBACK
};

void* msl_particle_probe_model(s32 gfx_id, const char* how, HSD_GObj* parent,
                               HSD_JObj* joint, const Vec3* translate,
                               const Vec3* rotate, const Vec3* scale,
                               const char* callback, int lifetime,
                               const Vec3* params, HSD_JObj* attach,
                               int scale_flags)
{
    const MslCoreEffectGeneratorBank* common = msl_effect_probe_bank(0);
    EF_Effect* effect;
    HSD_JObj* jobj;
    size_t i;

    // (the common file's models: the only table the simulator translates)
    if (probe_out == NULL || gfx_id < 0 || gfx_id >= 1000 || common == NULL ||
        common->models == NULL ||
        common->models[gfx_id].model_desc.joint == NULL)
    {
        return NULL;
    }
    pp_alloc_init(&pm_alloc_data, sizeof(EF_Effect), 4);
    pm_dat_entries[0].data = (void*) common->models;
    pm_anim_count = 0;
    msl_probe_heap = 1;
    if (strcmp(how, "create") == 0) {
        effect = efLib_Create(gfx_id, parent);
    } else if (strcmp(how, "attach") == 0 && joint != NULL) {
        effect = efLib_Create_Attach((u32) gfx_id, parent, joint);
    } else if (strcmp(how, "child") == 0 && joint != NULL) {
        effect = efLib_Create_AttachChild((u32) gfx_id, parent, joint);
    } else if (strcmp(how, "place") == 0) {
        effect = efLib_Create_Attach_Pos((u32) gfx_id, parent, (Vec3*) translate);
    } else {
        effect = NULL;
    }
    pm_anim_count = 0;
    msl_probe_heap = 0;
    if (effect == NULL || effect->gobj == NULL || effect->gobj->hsd_obj == NULL) {
        return NULL;
    }
    // what the dispatch then set on it (on the recorder's stand-in: only
    // what is no longer a stand-in's blank is the dispatch's)
    jobj = effect->gobj->hsd_obj;
    if (translate->x != 0.0F || translate->y != 0.0F || translate->z != 0.0F) {
        HSD_JObjSetTranslate(jobj, (Vec3*) translate);
    }
    if (rotate->x != 0.0F) { HSD_JObjSetRotationX(jobj, rotate->x); }
    if (rotate->y != 0.0F) { HSD_JObjSetRotationY(jobj, rotate->y); }
    if (rotate->z != 0.0F) { HSD_JObjSetRotationZ(jobj, rotate->z); }
    if (scale->x != 1.0F || scale->y != 1.0F || scale->z != 1.0F) {
        HSD_JObjSetScale(jobj, (Vec3*) scale);
    }
    for (i = 0; i < sizeof(pm_callbacks) / sizeof(pm_callbacks[0]); ++i) {
        if (strcmp(callback, pm_callbacks[i].name) == 0) {
            effect->update = pm_callbacks[i].fn;
        }
    }
    if (lifetime != 0) {
        effect->lifetime = (u16) lifetime;
    }
    effect->params = *params;
    if (attach != NULL) {
        effect->attach_jobj = attach;
    }
    if (scale_flags != 0) {
        effect->scale_flags = (u8) scale_flags;
    }
    return effect->gobj;
}

void msl_particle_probe_model_started(void* model, int frame, u32 seed_at)
{
    HSD_GObj* gobj = model;
    u32 saved;
    int i;

    if (probe_out == NULL || gobj == NULL) {
        return;
    }
    for (i = 0; i < PM_MODELS; ++i) {
        if (pm_models[i].gobj == gobj) {
            fprintf(probe_out, "[\"m\",%d,%d]\n", frame, pm_models[i].number);
        }
    }
    saved = *seed_ptr;
    *seed_ptr = seed_at;
    pm_step(gobj, frame, 1);
    *seed_ptr = saved;
}

static int pm_tree_used(const HSD_JObj* jobj)
{
    for (; jobj != NULL; jobj = jobj->next) {
        const HSD_Generator* gen;
        for (gen = hsd_804D78FC; gen != NULL; gen = gen->next) {
            if (gen->jobj == jobj) {
                return 1;
            }
        }
        if (!(jobj->flags & JOBJ_INSTANCE) && pm_tree_used(jobj->child)) {
            return 1;
        }
    }
    return 0;
}

// efLib_Update for each model (its life, its scale, its animation's step, its
// callback), then the ended ones' joints let go
static void pm_frame(int frame)
{
    int i;
    int k;
    int after = -1;

    // (in the order they were made: the lowest number not yet stepped)
    for (k = 0; k < PM_MODELS; ++k) {
        HSD_GObj* gobj;
        int best = -1;
        for (i = 0; i < PM_MODELS; ++i) {
            if (pm_models[i].gobj != NULL && pm_models[i].number > after &&
                (best < 0 || pm_models[i].number < pm_models[best].number))
            {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        i = best;
        after = pm_models[i].number;
        gobj = pm_models[i].gobj;
        if (!pm_models[i].dead) {
            pm_step(gobj, frame, 0);
        }
        if (pm_models[i].dead && !pm_tree_used(gobj->hsd_obj)) {
            msl_probe_heap = 1;
            HSD_JObjRemoveAll(gobj->hsd_obj);
            msl_probe_heap = 0;
            free(gobj->user_data);
            free(gobj);
            pm_models[i].gobj = NULL;
        }
    }
}

// efLib_DestroyAll's models: the owner's go, and the generators on their joints
static void pm_owner_ended(HSD_GObj* owner, int frame)
{
    int i;
    for (i = 0; i < PM_MODELS; ++i) {
        HSD_GObj* gobj = pm_models[i].gobj;
        if (gobj != NULL && !pm_models[i].dead &&
            GET_EFFECT(gobj)->parent_gobj == owner)
        {
            probe_end_tree(gobj->hsd_obj, frame);
            pm_models[i].dead = 1;
        }
    }
}

static void pm_reset(void)
{
    memset(pm_models, 0, sizeof(pm_models));
    pm_next_number = 0;
    pm_pending_count = 0;
    pm_collect = 0;
}

// --- the stage's generators, and the keys of the simulation's own joints -------
//
// refs/melee/src/melee/gr/grlib.c, refs/melee/src/melee/ef/eflib.c

#include "runtime/viewer_track.h"

float Ground_801C0498(void);

// probe_stage_kept: the one the stage's code keeps to end later (Final
// Destination's 0x7531, the Stadium's 0x7546 and 0x7544): the simulator
// hands the stage none, so the probe remembers it

static void probe_line(HSD_Generator* gen, int frame, s32 id, int how, u32 state)
{
    fprintf(probe_out, "[\"g\",%d,%d,%d,%d,%u,%d,%u,%u,%.9g,%.9g,%.9g", frame,
            probe_uid(gen), (int) id, (int) gen->linkNo, (unsigned) state, how,
            (unsigned) gen->type, (unsigned) gen->kind, gen->pos.x, gen->pos.y,
            gen->pos.z);
    probe_srt(gen->appsrt);
    fprintf(probe_out, ",%.9g]\n", gen->count);
}

void msl_particle_probe_stage(int how, s32 id, int bank, const Vec3* pos,
                              HSD_JObj* jobj, u32 state, int counted)
{
    HSD_Generator* gen = NULL;
    u32 saved;

    if (probe_out == NULL || !probe_bank(bank)) {
        return;
    }
    if (!probe_stage_told) {
        probe_stage_told = 1;
        fprintf(probe_out, "[\"stage\",%d]\n", (int) msl_particle_probe_stage_id());
    }
    saved = *seed_ptr;
    *seed_ptr = state;
    switch (how) {
    case MSL_PROBE_STAGE_PLACE:
        // grLib_801C96F8
        gen = hsd_8039F05C(0, bank, id);
        if (gen != NULL) {
            HSD_psAppSRT* srt = gen->appsrt;
            if (srt == NULL) {
                srt = psAddGeneratorAppSRT_begin(gen, 1);
            } else {
                srt->xA2 = 0;
            }
            if (srt == NULL) {
                hsd_8039D4DC(gen);
                gen = NULL;
            } else {
                float scale = Ground_801C0498();
                srt->translate = *pos;
                srt->scale.x *= scale;
                srt->scale.y *= scale;
                srt->scale.z *= scale;
                gen->pos.x = gen->pos.y = gen->pos.z = 0.0F;
            }
        }
        if (gen != NULL && (id == 0x7531 || id == 0x7546 || id == 0x7544)) {
            probe_stage_kept = gen;
        }
        break;
    case MSL_PROBE_STAGE_ONCE:
        gen = hsd_8039F6CC(0, bank, id, jobj);
        break;
    case MSL_PROBE_STAGE_JOINT:
        gen = hsd_8039EFAC(0, bank, id, jobj);
        break;
    default:
        break;
    }
    if (counted && *seed_ptr != state) {
        probe_drawn++;
    }
    *seed_ptr = saved;
    if (gen == NULL) {
        fprintf(probe_out, "[\"miss\",%d,%d]\n", (int) id, 6 + how);
        return;
    }
    probe_line(gen, msl_viewer_track_hook_frame(), id, 6 + how, state);
}

void msl_particle_probe_key(int bank, s32 id, HSD_JObj* jobj, u32 state)
{
    u32 saved;
    int i;

    if (probe_out == NULL || !probe_bank(bank)) {
        return;
    }
    saved = *seed_ptr;
    *seed_ptr = state;
    pm_collect = 1;
    pm_pending_count = 0;
    efLib_SpawnParticleEffect(bank, id, jobj, 1);
    pm_collect = 0;
    if (*seed_ptr != state) {
        probe_drawn++;
    }
    *seed_ptr = saved;
    for (i = 0; i < pm_pending_count; ++i) {
        if (probe_live(pm_pending[i])) {
            probe_line(pm_pending[i], msl_viewer_track_hook_frame(), id, 9, state);
        }
    }
    pm_pending_count = 0;
}

static void probe_stage_end_one(HSD_Generator* gen, int now, int frame)
{
    if (gen == NULL || !probe_live(gen)) {
        return;
    }
    fprintf(probe_out, "[\"e\",%d,%d,%d]\n", frame, probe_uid(gen), now ? 2 : 1);
    if (now) {
        gen->type |= 0x80;
    }
    hsd_8039D4DC(gen);
    // (one that lives on for its particles: off its joint, which is not the probe's to keep)
    if (probe_live(gen)) {
        gen->jobj = NULL;
    }
}

static void probe_stage_end_joint(HSD_JObj* jobj, int now, int tree, int frame)
{
    HSD_Generator* gen;
    HSD_Generator* next;

    if (jobj == NULL) {
        return;
    }
    for (gen = hsd_804D78FC; gen != NULL; gen = next) {
        next = gen->next;
        if (gen->jobj == jobj) {
            probe_stage_end_one(gen, now, frame);
            // (the list may have changed under the walk)
            next = hsd_804D78FC;
            while (next != NULL && next->jobj != jobj) {
                next = next->next;
            }
        }
    }
    if (!tree || (jobj->flags & JOBJ_INSTANCE)) {
        return;
    }
    for (jobj = jobj->child; jobj != NULL; jobj = jobj->next) {
        probe_stage_end_joint(jobj, now, tree, frame);
    }
}

void msl_particle_probe_stage_end(int how, HSD_JObj* jobj)
{
    int frame;

    if (probe_out == NULL) {
        return;
    }
    frame = msl_viewer_track_hook_frame();
    switch (how) {
    case MSL_PROBE_END_KEPT:
    case MSL_PROBE_END_KEPT_NOW:
        probe_stage_end_one(probe_stage_kept, how == MSL_PROBE_END_KEPT_NOW, frame);
        probe_stage_kept = NULL;
        break;
    case MSL_PROBE_END_JOINTS:
        probe_stage_end_joint(jobj, 0, 1, frame);
        break;
    case MSL_PROBE_END_JOINT_NOW:
        probe_stage_end_joint(jobj, 1, 0, frame);
        break;
    case MSL_PROBE_END_JOINTS_NOW:
        probe_stage_end_joint(jobj, 1, 1, frame);
        break;
    default:
        break;
    }
    fprintf(probe_out, "[\"x\",%d]\n", frame);
    probe_sweep();
}

void msl_particle_probe_joint_gone(HSD_JObj* jobj)
{
    HSD_Generator* gen;
    HSD_SList* waiting;

    // (the probe's own models' joints are freed by the probe, once no
    // generator is on them)
    if (probe_out == NULL || msl_probe_heap) {
        return;
    }
    for (gen = hsd_804D78FC; gen != NULL; gen = gen->next) {
        if (gen->jobj == jobj) {
            gen->jobj = NULL;
            fprintf(probe_out, "[\"j\",%d,%d]\n", msl_viewer_track_hook_frame(), probe_uid(gen));
        }
    }
    (void) waiting;
}

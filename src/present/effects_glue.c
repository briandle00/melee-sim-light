// Viewer build only (MSL_GX_RECORD): what the game's effect files and particle banks need from the host.
//
// The effect library (decomp melee/ef/{eflib,efasync,efsync,efalt}.c, vendored under decomp/) and the particle library (ps_lib.c, ps_disp.c) run
// as the game's. The game loads each effect file when a match asks for it and relocates its particle bank in place (efAsync_LoadSync ->
// lbArchive_80017040, psInitDataBank*), which cannot be done to a 32-bit file image on a 64-bit host. The simulator has translated every effect
// file's tables at GameData initialization (runtime/effects.c: models, command banks, texture groups); here each bank is made known to the
// particle library from them.
//
// refs/melee/src/melee/ef/efasync.c::{efAsync_LoadSync,efAsync_LoadAsync,efAsync_OnLoad},
//   refs/melee/src/sysdolphin/baselib/particle.c::{psInitDataBank,psInitDataBankLoad,psInitDataBankLocate}

// present-include: sysdolphin/baselib/probe_ps
// present-include: sysdolphin/baselib
#if MSL_GX_RECORD

#include <baselib/sislib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>

#include "probe_ps/psstructs.h"
#include "probe_ps/particle.h"

#include "ef/efasync.h"
#include "ef/types.h"
#include "runtime/effects.h"
#include "platform/native_dat.h"

extern EF_DAT_Entry efAsync_DatEntries[51];
extern u32* hsd_804D0948[65];

enum { MSL_PRESENT_BANKS = 65 };
static int bank_loaded[MSL_PRESENT_BANKS];
static HSD_PSCmdList** bank_commands[MSL_PRESENT_BANKS];
/* every texture group of a bank without a texture reads as absent */
static HSD_PSTexGroup* no_textures[256];

/* A bank as the library reads one (psInitDataBankLoad's result): the translated commands, each with its script (the bytes after its header
 * in the file), and the bank's texture groups. */
extern int msl_particle_probe_stage_id(void);
int msl_present_bank_load(int bank)
{
    const MslCoreEffectGeneratorBank* data = msl_effect_probe_bank(bank);
    int i;

    if (getenv("MSL_EF_TRACE") && bank == 30) fprintf(stderr, "load30 stage %d data=%p commands=%p count=%d%c", msl_particle_probe_stage_id(), (void*) data, data ? (void*) data->commands : NULL, data ? data->count : -1, 10);
    if (bank < 0 || bank >= MSL_PRESENT_BANKS || data == NULL || data->commands == NULL || data->count <= 0) {
        return 0;
    }
    if (bank_loaded[bank] && bank_commands[bank] != NULL) {
        return 1;
    }
    bank_commands[bank] = calloc((size_t) data->count, sizeof(HSD_PSCmdList*));
    /* (the library indexes by the full id; the simulator's array holds only the occupied range [id_base, count), at id - id_base) */
    for (i = data->id_base; i < data->count; ++i) {
        const HSD_PSCmdList* command = data->commands[i - data->id_base];
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
        /* psInitDataBankLocate: every command's kind is given these bits */
        copy->kind = (copy->kind & 0xF1FFFFFF) | 0x08000000;
        memcpy(copy->cmdList, script, size);
        /* (past its end, should a script run off it: the end command) */
        memset(copy->cmdList + size, 0xFF, 8);
        bank_commands[bank][i] = copy;
    }
    if (getenv("MSL_EF_TRACE")) {
        int made = 0, absent = 0;
        for (i = data->id_base; i < data->count; ++i) {
            if (bank_commands[bank][i] != NULL) made++; else absent++;
        }
        fprintf(stderr, "bank %d id_base %d count %d made %d absent %d\n", bank, data->id_base, data->count, made, absent);
    }
    ptclref_804D0E5C[bank] = bank_commands[bank];
    psCmdListArray[bank] = data->count;
    psTexGroupArray[bank] = data->textures != NULL ? data->textures : no_textures;
    psNumCmdList[bank] = NULL;
    hsd_804D0948[bank] = NULL;
    bank_loaded[bank] = 1;
    return 1;
}

/* efAsync_LoadSync / efAsync_LoadAsync: the file's table and its bank, from what the simulator translated */
void efAsync_LoadSync(int idx)
{
    const EF_EffectDesc* table;

    if (idx >= 50 || idx < 0 || efAsync_DatEntries[idx].ef_DAT_file == NULL) {
        return;
    }
    if (efAsync_DatEntries[idx].data != NULL) {
        return;
    }
    table = msl_effect_model_table(idx);
    if (table == NULL) {
        return;
    }
    efAsync_DatEntries[idx].data = (void*) table;
    msl_present_bank_load(idx);
}

void efAsync_LoadAsync(int idx)
{
    efAsync_LoadSync(idx);
}

/* psInitDataBank / psInitDataBankLoad: the only callers are the stage loader (Ground_801C0800: bank 0x1E, the stage's map_ptcl and map_texg),
 * whose bank the simulator has translated, and efAsync_LoadSync, replaced above */
void psInitDataBank(int bank, int* commands, int* textures, u32* references, int* forms)
{
    (void) commands;
    (void) textures;
    (void) references;
    (void) forms;
    msl_present_bank_load(bank);
}

void psInitDataBankLoad(int bank, const int* commands, const int* textures, const u32* references, const int* forms)
{
    (void) commands;
    (void) textures;
    (void) references;
    (void) forms;
    msl_present_bank_load(bank);
}

/* what a match's start makes ready: the common file and the particle library's tables */
void msl_present_effect_files_reset(void)
{
    int i;
    for (i = 0; i < MSL_PRESENT_BANKS; ++i) {
        bank_loaded[i] = 0;
        if (i < 51) {
            efAsync_DatEntries[i].data = NULL;
        }
    }
}

#endif

#if MSL_GX_RECORD
#include "ef/eflib.h"
#include "lb/lbrefract.h"

/* fn_8016E730: lbRefract_800222A4(); lb_8000FCDC(); efLib_Init(); efAsync_LoadSync(0); efAsync_LoadSync(0x1F); (the match scene's start; the effect
 * library's two objects, its callbacks and the common file) */

void msl_present_effects_init(void)
{
    /* the text engine starts at the console's boot (HSD_SisLib_803A6048); here it is per match, as the objects it holds are */
    HSD_SisLib_803A6048(0);
    msl_present_effect_files_reset();
    lbRefract_800222A4();
    efLib_Init();
    efAsync_LoadSync(0);
    efAsync_LoadSync(0x1F);
}
#endif

#if MSL_GX_RECORD
/* MWCC's runtime: a double converted to an unsigned int (saturating), which the compiler calls for `(u32) f` with f a float variable.
 * refs/melee/extern/runtime (Runtime/__va_arg.c's neighbors: __cvt_fp2unsigned) */
unsigned int __cvt_fp2unsigned(double d)
{
    if (!(d > 0.0)) {
        return 0u;
    }
    if (d >= 4294967296.0) {
        return 0xFFFFFFFFu;
    }
    return (unsigned int) d;
}
#endif

#if MSL_GX_RECORD
#include "ft/fighter.h"
#include "ft/types.h"
#include "runtime/context.h"
#include "runtime/scalar.h"

extern u8 ftData_UnkBytePerCharacter[];

/* each fighter's loader asks for its effect file (efAsync_LoadSync(ftData_UnkBytePerCharacter[kind])); the simulator's loaders do not call it
 * (the files are translated at GameData initialization), so the match's fighters' files are made known here, once the fighters exist */
void msl_present_load_fighter_banks(MslCoreMatch* match)
{
    int i;
    for (i = 0; i < match->config.num_players; ++i) {
        HSD_GObj* both[2] = { match->fighters[i], match->follower_fighters[i] };
        int k;
        for (k = 0; k < 2; ++k) {
            if (both[k] != NULL) {
                efAsync_LoadSync(ftData_UnkBytePerCharacter[GET_FIGHTER(both[k])->kind]);
            }
        }
    }
}
#endif

#if MSL_GX_RECORD
/* A float vararg slot of efAsync_Spawn: retail passes an f32*; some call sites of the decomp pass the float itself, which arrives promoted to a
 * double. A plausible pointer is read through; otherwise the slot is taken as a double (runtime/effects.c::msl_effect_float_arg, the stand-in's). */
float msl_present_float_arg(unsigned long long bits)
{
    double d;

    /* (the slot is 64 bits on every host: the shift is not on a pointer-sized type, which is 32 bits in WebAssembly) */
    if (bits >= 0x10000u && (bits & 3u) == 0 && bits < ((unsigned long long) 1 << 47)) {
        return *(const float*) (void*) (uintptr_t) bits;
    }
    memcpy(&d, &bits, sizeof(d));
    if (d == d && d > -1.0e7 && d < 1.0e7) {
        return (float) d;
    }
    return 0.0F;
}
#endif

#if MSL_GX_RECORD
/* a debugging aid: MSL_PTCL_DUMP=<first step>:<last step> lists the particles alive at the end of those steps (the game's list heads, hsd_804D0908) */
extern void* hsd_804D0908[16];
static void dump_particles(int step, const char* env_name, int* plo, int* phi);
/* (MSL_PTCL_DUMP_POST=<a>:<b>: the same after the frame's display pass, which computes the appsrt matrices) */
void msl_present_dump_particles_post(int step)
{
    static int lo = -2, hi = -2;
    dump_particles(step, "MSL_PTCL_DUMP_POST", &lo, &hi);
#undef lo
#undef hi
}

void msl_present_dump_particles(int step)
{
    static int lo = -2, hi = -2;
    dump_particles(step, "MSL_PTCL_DUMP", &lo, &hi);
}

static void dump_particles(int step, const char* env_name, int* plo, int* phi)
{
    int i;
#define lo (*plo)
#define hi (*phi)

    if (lo == -2) {
        const char* v = getenv(env_name);
        lo = hi = -1;
        if (v != NULL) {
            sscanf(v, "%d:%d", &lo, &hi);
        }
    }
    if (step < lo || step > hi) {
        return;
    }
    {
        extern void* hsd_804D78FC;
        HSD_Generator* g = hsd_804D78FC;
        for (; g != NULL; g = g->next) {
            fprintf(stderr, "GEN step %d kind %x random %g count %.6f genLife %d type %x bank %d link %d id %d life %d pos %g %g %g radius %g angle %.6f aobj %g/%g\n", step, (unsigned) g->kind, g->random, g->count, g->genLife, (unsigned) g->type, g->bank, g->linkNo, g->idnum, g->life, g->pos.x, g->pos.y, g->pos.z, g->radius, g->angle, g->jobj && g->jobj->aobj ? g->jobj->aobj->curr_frame : -1.0f, g->jobj && g->jobj->aobj ? g->jobj->aobj->end_frame : -1.0f);
            { unsigned *u = (unsigned*) &g->pos; unsigned *v = (unsigned*) &g->vel; unsigned *rr = (unsigned*) &g->radius; unsigned *aa = (unsigned*) &g->angle;
              fprintf(stderr, "GENH id %d bank %d type %x kind %x life %d genLife %d pos %08x %08x %08x vel %08x %08x %08x rad %08x ang %08x%c", g->idnum, g->bank, (unsigned) g->type, (unsigned) g->kind, g->life, g->genLife, u[0], u[1], u[2], v[0], v[1], v[2], *rr, *aa, 10); }
            { const char* want = getenv("MSL_GEN_CHAIN"); if (want != NULL && (strcmp(want, "all") == 0 || g->idnum == atoi(want)) && g->jobj != NULL) { HSD_JObj* j = g->jobj; for (; j != NULL; j = j->parent) { unsigned *w = (unsigned*) &j->rotate; unsigned *sc = (unsigned*) &j->scale; unsigned *tr = (unsigned*) &j->translate; unsigned *mm = (unsigned*) &j->mtx; int k;
                fprintf(stderr, "JOBJ gen %d flags %08x aobj %d rot %08x %08x %08x %08x scale %08x %08x %08x trans %08x %08x %08x mtx", g->idnum, (unsigned) j->flags, j->aobj != NULL, w[0], w[1], w[2], w[3], sc[0], sc[1], sc[2], tr[0], tr[1], tr[2]);
                for (k = 0; k < 12; ++k) fprintf(stderr, " %08x", mm[k]);
                fprintf(stderr, "%c", 10); } fprintf(stderr, "ENDCHAIN%c", 10); } }
        }
    }
    for (i = 0; i < 16; ++i) {
        HSD_Particle* p = hsd_804D0908[i];
        int n = 0;
        for (; p != NULL; p = p->next, ++n) {
            { union { float f; unsigned u; } a, b, c, d, e, f, g; a.f = p->pos.x; b.f = p->pos.y; c.f = p->pos.z; d.f = p->vel.x; e.f = p->vel.y; f.f = p->vel.z; g.f = p->size;
              fprintf(stderr, "PTCL list %d #%d bank %d id %d life %d cmdPtr %d pos %08x %08x %08x vel %08x %08x %08x size %08x rot %08x x kind %08x szc %d szt %08x rotc %d rott %08x pc %08x ec %08x wait %d grav %08x fric %08x trail %08x pose %d", i, n, p->bank, p->idnum, p->life, p->cmdPtr, a.u, b.u, c.u, d.u, e.u, f.u, g.u, *(unsigned*) &p->rotate, (unsigned) p->kind, (int) p->sizeCount, *(unsigned*) &p->sizeTarget, (int) p->rotateCount, *(unsigned*) &p->rotateTarget, ((unsigned) p->primCol.r << 24) | ((unsigned) p->primCol.g << 16) | ((unsigned) p->primCol.b << 8) | (unsigned) p->primCol.a, ((unsigned) p->envCol.r << 24) | ((unsigned) p->envCol.g << 16) | ((unsigned) p->envCol.b << 8) | (unsigned) p->envCol.a, (int) p->cmdWait, *(unsigned*) &p->grav, *(unsigned*) &p->fric, *(unsigned*) &p->trail, (int) p->poseNum); { HSD_psAppSRT* ap = p->appsrt; if (ap == NULL) fprintf(stderr, " ap -%c", 10); else { unsigned* q = (unsigned*) &ap->translate; unsigned* r = (unsigned*) &ap->rot; unsigned* sc = (unsigned*) &ap->scale; unsigned* mm = (unsigned*) &ap->mmtx; int k;
                fprintf(stderr, " ap st %d t %08x %08x %08x r %08x %08x %08x %08x s %08x %08x %08x m", (int) ap->status, q[0], q[1], q[2], r[0], r[1], r[2], r[3], sc[0], sc[1], sc[2]);
                for (k = 0; k < 12; ++k) fprintf(stderr, " %08x", mm[k]);
                fprintf(stderr, "%c", 10); } } }
        }
    }
}
#endif

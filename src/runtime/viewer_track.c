#include "runtime/viewer_track.h"

#include "cm/camera.h"
#include "ft/fighter.h"
#include "ft/chara/ftCommon/ftCo_DamageFall.h"
#include "ft/ft_0DF0.h"
#include "ft/ftlib.h"
#include "ft/types.h"
#include "if/ifmagnify.h"
#include "gr/grdatfiles.h"
#include "gr/ground.h"
#include "gr/inlines.h"
#include "gr/types.h"
#include "ft/ftdynamics.h"
#include "it/inlines.h"
#include "it/itCharItems.h"
#include "it/types.h"
#include "lb/types.h"
#include "pl/player.h"
#include "runtime/context.h"
#include "runtime/effect_pieces.h"
#include "runtime/effects.h"
#include "runtime/ps_probe.h"
#include "runtime/scalar.h"
#include "runtime/fighter_pose.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <baselib/gobj.h>
#include <baselib/gobjproc.h>
#include <baselib/jobj.h>
#include <baselib/mtx.h>
#include <baselib/random.h>
#include <baselib/robj.h>

// The game camera's render transform (cm/camera.c), read only.
void msl_camera_get_render_transform(Vec3* position, Vec3* interest,
                                     float* fov);
void msl_camera_track_extras(Vec2* translation, u8* background_rgb);

int msl_viewer_track_on;

// scalar.c's external stage ids.
enum {
    TRACK_STAGE_BATTLEFIELD = 31,
    TRACK_STAGE_FINAL_DESTINATION = 32,
    TRACK_STAGE_FOUNTAIN = 2,
};

static FILE* track_out;

// A probe (not part of the track): each fighter part's joint as the game
// posed it every frame, to the binary file MSL_POSE_WORLD names - the
// reference the player's poses are measured against (tools/pose_parity.ts:
// laid on the player's own skeleton). Each record: int16 frame, port, sub,
// parts; then per part a uint32 of its flags (JOBJ_USE_QUATERNION: its turn
// a quaternion) and its turn x, y, z, w, scale x, y, z and place x, y, z as
// floats, in its parent's frame. (Its world matrix is no reference: the
// headless core makes only the ones gameplay reads.)
static FILE* world_out;

// Last value written for each state key, so a state is written only when it
// changes. Keys are the type and identity fields of the line.
enum { TRACK_KEYS = 2048, TRACK_KEY_LEN = 48, TRACK_VALUE_LEN = 200 };
static struct {
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];
} track_state[TRACK_KEYS];

// (a stage part's tree runs to a few hundred joints: a joint past the last
// is not written)
enum { TRACK_POSE_OWNERS = 128, TRACK_POSE_JOINTS = 512, TRACK_WORLD_OWNERS = 64 };

// One owner's joints as last written (track_poses): an item, a fighter, a
// chain's links, a stage part.
typedef struct MslTrackPose {
    unsigned char used;
    char kind;
    int id;
    int frame;
    int character;
    unsigned char known[TRACK_POSE_JOINTS];
    unsigned char flag[TRACK_POSE_JOINTS];
    float last[TRACK_POSE_JOINTS][12];
} MslTrackPose;

// The poses last written, per owner (track_poses), and the line being made.
// (The public stack keeps these per match, in the record its viewer API
// makes; this driver writes one track at a time, so there is one.)
typedef struct MslViewerTrack {
    MslTrackPose pose[TRACK_POSE_OWNERS];
    char pose_line[256 * 1024];
    // (track_fighter_joints: the parts on a chain this build leaves unsolved)
    uint8_t chain_parts[TRACK_POSE_JOINTS];
    // The world matrices ("wf", "wi", "ws": track_world), only when the
    // environment sets MSL_VIEWER_TRACK_WORLD=1 as the track is made: their
    // owners, allocated with the track, else NULL.
    MslTrackPose* world;
    size_t pose_used;
    // Which fighters (port * 2 + sub) had a "pos" line the frame before:
    // the frame one comes back to the plane writes one more.
    unsigned char pos_off[12];
} MslViewerTrack;
static MslViewerTrack track_record;

// A float as the track writes it: its IEEE single's bits, as "0x%08x" (no
// decimal rounding, so every host's C library writes the same line).
static unsigned vt_f32(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return (unsigned) bits;
}

#define put(track, ...) ((void) (track), fprintf(track_out, __VA_ARGS__))

static void track_hud_reset(void);

// The items' joints something was made on (a generator of the item's own
// animation, a piece of an effect): the item, the joint's preorder index in
// its tree, and the last "ij" written for it. One is dropped with its item.
enum { TRACK_ITEM_JOINTS = 128 };
static struct {
    int item;
    int joint;
    char value[TRACK_VALUE_LEN];
} track_ij[TRACK_ITEM_JOINTS];
static int track_ij_count;

void msl_viewer_track_begin(FILE* out)
{
    track_out = out;
    msl_viewer_track_on = out != NULL;
    memset(track_state, 0, sizeof(track_state));
    track_ij_count = 0;
    free(track_record.world);
    memset(&track_record, 0, sizeof(track_record));
    {
        // (the pose check's world matrices: only when asked for)
        const char* world = getenv("MSL_VIEWER_TRACK_WORLD");
        if (out != NULL && world != NULL && strcmp(world, "1") == 0) {
            track_record.world = calloc(TRACK_WORLD_OWNERS, sizeof(*track_record.world));
        }
    }
    track_hud_reset();
    if (world_out != NULL) {
        fclose(world_out);
        world_out = NULL;
    }
    if (out != NULL && getenv("MSL_POSE_WORLD") != NULL) {
        world_out = fopen(getenv("MSL_POSE_WORLD"), "wb");
    }
    if (out != NULL) {
        msl_particle_probe_begin();
    }
}

void msl_viewer_track_end(void)
{
    free(track_record.world);
    track_record.world = NULL;
    if (track_out != NULL) {
        fflush(track_out);
    }
    if (world_out != NULL) {
        fclose(world_out);
        world_out = NULL;
    }
    track_out = NULL;
    msl_viewer_track_on = 0;
    msl_particle_probe_end();
}

// The frame a hook fires in: the match advances frame_id after the gameplay
// procs, so a hook inside a step (or construction) belongs to frame_id + 1.
static int hook_frame(void)
{
    MslCoreMatch* match = msl_core_active_match();
    return match != NULL ? match->frame_id + 1 : -123;
}

static unsigned hash_key(const char* key)
{
    unsigned h = 2166136261U;
    while (*key) {
        h = (h ^ (unsigned char) *key++) * 16777619U;
    }
    return h;
}

// Writes [frame,key,value] when value differs from the last one for key.
static void emit_state(int frame, const char* key, const char* value)
{
    unsigned slot = hash_key(key) % TRACK_KEYS;
    unsigned probe;

    for (probe = 0; probe < TRACK_KEYS; ++probe) {
        unsigned i = (slot + probe) % TRACK_KEYS;
        if (track_state[i].key[0] == '\0') {
            snprintf(track_state[i].key, TRACK_KEY_LEN, "%s", key);
            snprintf(track_state[i].value, TRACK_VALUE_LEN, "%s", value);
            fprintf(track_out, "[%d,%s%s]\n", frame, key, value);
            return;
        }
        if (strcmp(track_state[i].key, key) == 0) {
            if (strcmp(track_state[i].value, value) != 0) {
                snprintf(track_state[i].value, TRACK_VALUE_LEN, "%s", value);
                fprintf(track_out, "[%d,%s%s]\n", frame, key, value);
            }
            return;
        }
    }
}

// The last value written for key, or NULL.
static const char* last_state(const char* key)
{
    unsigned slot = hash_key(key) % TRACK_KEYS;
    unsigned probe;

    for (probe = 0; probe < TRACK_KEYS; ++probe) {
        unsigned i = (slot + probe) % TRACK_KEYS;
        if (track_state[i].key[0] == '\0') {
            return NULL;
        }
        if (strcmp(track_state[i].key, key) == 0) {
            return track_state[i].value;
        }
    }
    return NULL;
}

static uint32_t rgba(GXColor c)
{
    return ((uint32_t) c.r << 24) | ((uint32_t) c.g << 16) |
           ((uint32_t) c.b << 8) | (uint32_t) c.a;
}

int msl_viewer_track_hook_frame(void)
{
    return hook_frame();
}

// The draws the game has made this frame that the simulator leaves out: the
// stage's own generator calls (grlib.c makes none here) and the particle
// keys of joints outside the stage's file. A frame's last state depends
// only on how many draws there were, so the simulator's is stepped by them.
static int track_skipped;

static uint32_t seed_now(void)
{
    uint32_t state = *seed_ptr;
    int i;
    for (i = 0; i < track_skipped; ++i) {
        state = state * 214013 + 2531011;
    }
    return state;
}

u32 msl_viewer_track_state(void)
{
    return seed_now();
}

int msl_viewer_track_plain(void)
{
    static int plain = -1;
    if (plain < 0) {
        const char* on = getenv("MSL_TRACK_NO_STEP");
        plain = on != NULL && on[0] != '\0';
    }
    return plain;
}

void msl_viewer_track_skipped_n(int n)
{
    if (!msl_viewer_track_plain() && track_out != NULL && n > 0) {
        track_skipped += n;
    }
}

void msl_viewer_track_skipped(int bank, int generator_id)
{
    if (!msl_viewer_track_plain() && track_out != NULL &&
        msl_effect_generator_draws(bank, generator_id))
    {
        track_skipped++;
    }
}

// Owner of an effect: kind letter, id, and the JObj's index in it (-1 when
// the JObj is not one of its joints or there is none).
static void describe_owner(HSD_GObj* gobj, HSD_JObj* jobj, char* kind,
                           int* id, int* joint)
{
    *kind = '-';
    *id = -1;
    *joint = -1;
    if (gobj == NULL) {
        return;
    }
    switch (gobj->classifier) {
    case HSD_GOBJ_CLASS_FIGHTER: {
        Fighter* fp = GET_FIGHTER(gobj);
        u32 i;
        *kind = 'f';
        *id = fp->player_id * 2 + (fp->x221F_b4 ? 1 : 0);
        if (jobj != NULL && fp->parts != NULL) {
            for (i = 0; i < ftPartsTable[fp->kind]->parts_num; ++i) {
                if (fp->parts[i].joint == jobj) {
                    *joint = (int) i;
                    break;
                }
            }
        }
        break;
    }
    case HSD_GOBJ_CLASS_ITEM: {
        Item* ip = GET_ITEM(gobj);
        int i;
        *kind = 'i';
        *id = (int) ip->x1C;
        if (jobj != NULL && ip->xBBC_dynamicBoneTable != NULL) {
            for (i = 0; i < (int) (sizeof(ip->xBBC_dynamicBoneTable->bones) /
                                   sizeof(ip->xBBC_dynamicBoneTable->bones[0]));
                 ++i)
            {
                if (ip->xBBC_dynamicBoneTable->bones[i] == jobj) {
                    *joint = i;
                    break;
                }
            }
        }
        break;
    }
    case HSD_GOBJ_CLASS_STAGE: {
        Ground* gp = GET_GROUND(gobj);
        *kind = 's';
        *id = (int) gp->map_id;
        break;
    }
    default:
        break;
    }
}

void msl_viewer_track_fx(HSD_GObj* owner, s32 gfx_id, HSD_JObj* jobj,
                         const Vec3* pos, float a0, float a1)
{
    char kind;
    int id;
    int joint;
    Vec3 at = { 0.0F, 0.0F, 0.0F };

    if (track_out == NULL) {
        return;
    }
    if (pos != NULL) {
        at = *pos;
    }
    describe_owner(owner, jobj, &kind, &id, &joint);
    fprintf(track_out,
            "[%d,\"fx\",%d,\"%c\",%d,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%u]\n",
            hook_frame(), (int) gfx_id, kind, id, joint, at.x, at.y, at.z,
            a0, a1, (unsigned) seed_now());
}

// [frame,"fxp",gfx_id,owner kind,owner id,index,"m"|"g",id,how,joint,
//  tx,ty,tz,rx,ry,rz,sx,sy,sz,callback,lifetime,p0,p1,p2,mirrored,
//  ax,ay,az: where it was made, its joint's world position for one on a joint,
//  seed: the random state as it was made and, for a generator made on an
//  item's joint, the item and the joint's preorder index in its tree (its
//  "ij" events)]
static int item_joint(HSD_JObj* jobj, int* item, int* joint);

void msl_viewer_track_fxp(HSD_GObj* owner, s32 gfx_id,
                          const MslEffectPiece* p)
{
    char kind;
    int id;
    int joint;
    int item;
    int item_at;
    char more[32] = "";

    if (track_out == NULL) {
        return;
    }
    describe_owner(owner, p->joint, &kind, &id, &joint);
    if (p->kind == 'g' && p->joint != NULL && !msl_viewer_track_plain() &&
        item_joint(p->joint, &item, &item_at))
    {
        snprintf(more, sizeof(more), ",%d,%d", item, item_at);
    }
    fprintf(track_out,
            "[%d,\"fxp\",%d,\"%c\",%d,%d,\"%c\",%d,\"%s\",%d,"
            "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,\"%s\",%d,"
            "%.6g,%.6g,%.6g,%d,%.6g,%.6g,%.6g,%u%s]\n",
            hook_frame(), (int) gfx_id, kind, id, p->index, p->kind,
            (int) p->id, p->how, joint, p->t.x, p->t.y, p->t.z, p->r.x,
            p->r.y, p->r.z, p->s.x, p->s.y, p->s.z, p->callback,
            p->lifetime, p->params.x, p->params.y, p->params.z,
            p->mirrored, p->at.x, p->at.y, p->at.z, (unsigned) p->rng_at,
            more);
}

void msl_viewer_track_fxend(HSD_GObj* owner)
{
    char kind;
    int id;
    int joint;

    if (track_out == NULL) {
        return;
    }
    describe_owner(owner, NULL, &kind, &id, &joint);
    fprintf(track_out, "[%d,\"fxend\",\"%c\",%d]\n", hook_frame(), kind, id);
    msl_particle_probe_owner_ended(owner, hook_frame());
}

// Where a joint is among the stage's: its stage object's place in the
// stage's list (Ground_801C2BA4) and its preorder index in that object's
// tree (the root 0, a child before the next). -1, -1 for none.
static int stage_joint_walk(HSD_JObj* at, HSD_JObj* want, int* count)
{
    for (; at != NULL; at = at->next) {
        if (at == want) {
            return 1;
        }
        ++*count;
        if (!(at->flags & JOBJ_INSTANCE) && stage_joint_walk(at->child, want, count)) {
            return 1;
        }
    }
    return 0;
}

static int stage_joint_in(HSD_GObj* gobj, HSD_JObj* jobj)
{
    HSD_JObj* root = gobj != NULL ? gobj->hsd_obj : NULL;
    int count = 1;
    if (root == NULL) {
        return -1;
    }
    if (root == jobj) {
        return 0;
    }
    return stage_joint_walk(root->child, jobj, &count) ? count : -1;
}

static void stage_joint(HSD_JObj* jobj, int* map, int* joint)
{
    MslCoreMatch* match = msl_core_try_active_match();
    HSD_GObj* gobj;
    int others[64];
    int i;
    *map = -1;
    *joint = -1;
    for (i = 0; jobj != NULL && i < 64; ++i) {
        int at = stage_joint_in(stage_info.x180[i], jobj);
        if (at >= 0) {
            *map = i;
            *joint = at;
            return;
        }
    }
    // A stage object the stage's table does not hold: the table keeps one
    // object a number, the last made, and a stage's code can make more
    // (Fountain of Dreams' two water columns are both number 4: the table has
    // the right one's, made second). Such an object is named by its number
    // plus 1000 for each one of that number made before it and still there,
    // itself counted: 1004 is the first column.
    // refs/melee/src/melee/gr/{ground.c::Ground_801C14D0,grizumi.c::grIzumi_801CCBDC}
    if (jobj == NULL || match == NULL || match->gobj.entities == NULL) {
        return;
    }
    memset(others, 0, sizeof(others));
    for (gobj = (HSD_GObj*) match->gobj.entities->x14; gobj != NULL; gobj = gobj->next) {
        Ground* gp;
        int id;
        int at;
        if (gobj->classifier != HSD_GOBJ_CLASS_STAGE || gobj->user_data == NULL) {
            continue;
        }
        gp = GET_GROUND(gobj);
        id = (int) gp->map_id;
        if (id < 0 || id >= 64 || stage_info.x180[id] == gobj) {
            continue;
        }
        ++others[id];
        at = stage_joint_in(gobj, jobj);
        if (at >= 0) {
            *map = id + 1000 * others[id];
            *joint = at;
            return;
        }
    }
}

// Whose joint it is, a fighter's or an item's (the top of its tree is the
// owner's model): the owner as describe_owner names it, '-' for neither.
static void joint_owner(HSD_JObj* jobj, char* kind, int* id)
{
    MslCoreMatch* match = msl_core_try_active_match();
    HSD_JObj* top = jobj;
    HSD_GObj* gobj;
    int joint;
    int n;

    *kind = '-';
    *id = -1;
    if (jobj == NULL || match == NULL || match->gobj.entities == NULL) {
        return;
    }
    for (n = 0; top->parent != NULL && n < 256; ++n) {
        top = top->parent;
    }
    for (gobj = (HSD_GObj*) match->gobj.entities->fighters; gobj != NULL; gobj = gobj->next) {
        if (gobj->hsd_obj == top) {
            describe_owner(gobj, NULL, kind, id, &joint);
            return;
        }
    }
    for (gobj = (HSD_GObj*) match->gobj.entities->items; gobj != NULL; gobj = gobj->next) {
        if (gobj->hsd_obj == top) {
            describe_owner(gobj, NULL, kind, id, &joint);
            return;
        }
    }
}

// The item whose joint it is and the joint's preorder index in the item's
// tree (the root 0, as stage_joint_in counts), noted for "ij": 1 when it is
// an item's. The joint is only named here; its matrix is read at the frame's
// end (track_item_joints), where the game's particles read it.
static int item_joint(HSD_JObj* jobj, int* item, int* joint)
{
    MslCoreMatch* match = msl_core_try_active_match();
    HSD_JObj* top = jobj;
    HSD_GObj* gobj;
    int n;
    int i;

    if (jobj == NULL || match == NULL || match->gobj.entities == NULL) {
        return 0;
    }
    for (n = 0; top->parent != NULL && n < 256; ++n) {
        top = top->parent;
    }
    for (gobj = (HSD_GObj*) match->gobj.entities->items; gobj != NULL; gobj = gobj->next) {
        if (gobj->hsd_obj == top && gobj->user_data != NULL) {
            int at = stage_joint_in(gobj, jobj);
            if (at < 0) {
                return 0;
            }
            *item = (int) GET_ITEM(gobj)->x1C;
            *joint = at;
            for (i = 0; i < track_ij_count; ++i) {
                if (track_ij[i].item == *item && track_ij[i].joint == at) {
                    return 1;
                }
            }
            if (track_ij_count < TRACK_ITEM_JOINTS) {
                track_ij[track_ij_count].item = *item;
                track_ij[track_ij_count].joint = at;
                track_ij[track_ij_count].value[0] = '\0';
                ++track_ij_count;
            }
            return 1;
        }
    }
    return 0;
}

// The joint at a preorder index of a tree (stage_joint_walk's count).
static HSD_JObj* joint_at_walk(HSD_JObj* at, int want, int* count)
{
    for (; at != NULL; at = at->next) {
        HSD_JObj* found;
        if (*count == want) {
            return at;
        }
        ++*count;
        if (!(at->flags & JOBJ_INSTANCE) &&
            (found = joint_at_walk(at->child, want, count)) != NULL)
        {
            return found;
        }
    }
    return NULL;
}

// [frame,"ij",item,joint,ox,oy,oz,m00,m01,m02,m10,m11,m12,m20,m21,m22]: a
// noted item joint's place from the item's own (ip->pos) and its matrix's
// turn and size, at the frame's end, written when it changes. A generator on
// the joint goes to its place each frame and its emission is turned by it
// (hsd_8039EFAC; generator.c's update), and what its particles make is on
// the same joint. The matrix is set up here as the game's draw would have.
static void track_item_joints(int frame, const MslCoreMatch* match)
{
    int i = 0;
    while (i < track_ij_count) {
        HSD_GObj* gobj;
        HSD_JObj* jobj = NULL;
        Item* ip = NULL;
        char value[TRACK_VALUE_LEN];
        for (gobj = (HSD_GObj*) match->gobj.entities->items; gobj != NULL; gobj = gobj->next) {
            if (gobj->user_data != NULL && (int) GET_ITEM(gobj)->x1C == track_ij[i].item) {
                HSD_JObj* root = gobj->hsd_obj;
                int count = 1;
                ip = GET_ITEM(gobj);
                jobj = root == NULL ? NULL
                       : track_ij[i].joint == 0 ? root
                       : joint_at_walk(root->child, track_ij[i].joint, &count);
                break;
            }
        }
        if (jobj == NULL) {
            track_ij[i] = track_ij[--track_ij_count];
            continue;
        }
        HSD_JObjSetupMatrix(jobj);
        snprintf(value, sizeof(value),
                 ",%.4f,%.4f,%.4f,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g",
                 jobj->mtx[0][3] - ip->pos.x, jobj->mtx[1][3] - ip->pos.y,
                 jobj->mtx[2][3] - ip->pos.z, jobj->mtx[0][0], jobj->mtx[0][1],
                 jobj->mtx[0][2], jobj->mtx[1][0], jobj->mtx[1][1],
                 jobj->mtx[1][2], jobj->mtx[2][0], jobj->mtx[2][1],
                 jobj->mtx[2][2]);
        if (strcmp(track_ij[i].value, value) != 0) {
            snprintf(track_ij[i].value, TRACK_VALUE_LEN, "%s", value);
            fprintf(track_out, "[%d,\"ij\",%d,%d%s]\n", frame, track_ij[i].item,
                    track_ij[i].joint, value);
        }
        ++i;
    }
}

// [frame,"gen",bank,id,x,y,z,seed,stage object,joint] and, for a joint that is
// not the stage's, the owner whose joint it is: ,"f"|"i",owner and, for an
// item's, the joint's preorder index in the item's tree (its "ij" events)
void msl_viewer_track_gen(int bank, int gen_id, HSD_JObj* jobj)
{
    int map;
    int joint;
    char owner_kind;
    int owner_id;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    if (track_out == NULL) {
        return;
    }
    if (jobj != NULL) {
        x = jobj->mtx[0][3];
        y = jobj->mtx[1][3];
        z = jobj->mtx[2][3];
    }
    if (msl_viewer_track_plain()) {
        fprintf(track_out, "[%d,\"gen\",%d,%d,%.6g,%.6g,%.6g,%u]\n",
                hook_frame(), bank, gen_id, x, y, z, (unsigned) seed_now());
        return;
    }
    stage_joint(jobj, &map, &joint);
    if (map < 0) {
        joint_owner(jobj, &owner_kind, &owner_id);
        if (owner_kind != '-') {
            int item;
            int item_at;
            if (owner_kind == 'i' && item_joint(jobj, &item, &item_at) &&
                item == owner_id)
            {
                fprintf(track_out,
                        "[%d,\"gen\",%d,%d,%.6g,%.6g,%.6g,%u,%d,%d,\"%c\",%d,%d]\n",
                        hook_frame(), bank, gen_id, x, y, z,
                        (unsigned) seed_now(), map, joint, owner_kind,
                        owner_id, item_at);
                return;
            }
            fprintf(track_out,
                    "[%d,\"gen\",%d,%d,%.6g,%.6g,%.6g,%u,%d,%d,\"%c\",%d]\n",
                    hook_frame(), bank, gen_id, x, y, z,
                    (unsigned) seed_now(), map, joint, owner_kind, owner_id);
            return;
        }
    }
    fprintf(track_out, "[%d,\"gen\",%d,%d,%.6g,%.6g,%.6g,%u,%d,%d]\n",
            hook_frame(), bank, gen_id, x, y, z, (unsigned) seed_now(), map,
            joint);
}

void msl_viewer_track_ganim(HSD_GObj* gobj, int anim_set, int anim_index,
                            float start_frame, float rate)
{
    if (track_out == NULL || gobj == NULL ||
        gobj->classifier != HSD_GOBJ_CLASS_STAGE)
    {
        return;
    }
    fprintf(track_out, "[%d,\"ganim\",%d,%d,%d,%.6g,%.6g]\n", hook_frame(),
            (int) GET_GROUND(gobj)->map_id, anim_set, anim_index, start_frame,
            rate);
}

void msl_viewer_track_stadscreen(int mode, int player)
{
    if (track_out == NULL) {
        return;
    }
    fprintf(track_out, "[%d,\"stadscreen\",%d,%d]\n", hook_frame(), mode,
            mode == 8 ? player : -1);
}

void msl_viewer_track_bgflash(int type, int duration)
{
    if (track_out == NULL) {
        return;
    }
    fprintf(track_out, "[%d,\"bgflash\",%d,%d]\n", hook_frame(), type, duration);
}

void msl_viewer_track_bird(int kind, int count, const Vec3* pos)
{
    if (track_out == NULL) {
        return;
    }
    fprintf(track_out, "[%d,\"bird\",%d,%d,%.6g,%.6g,%.6g]\n", hook_frame(),
            kind, count, pos->x, pos->y, pos->z);
}

// The stage object whose proc is running, or -1 (construction, or a proc
// that is not a stage's).
static int running_map_id(void)
{
    HSD_GObjProc* proc = HSD_GObj_804D7838;
    if (proc != NULL && proc->gobj != NULL &&
        proc->gobj->classifier == HSD_GOBJ_CLASS_STAGE)
    {
        return (int) GET_GROUND(proc->gobj)->map_id;
    }
    return -1;
}

// How many joints an end of a joint's tree walks (grLib_801C9854,
// grLib_801C9908): the joint and what is under it, an instance's children
// left out - the same joints, in the same order, stage_joint counts.
static int stage_tree_count(HSD_JObj* jobj)
{
    int n = 1;
    HSD_JObj* child;
    if (jobj == NULL) {
        return 0;
    }
    if (!(jobj->flags & JOBJ_INSTANCE)) {
        for (child = jobj->child; child != NULL; child = child->next) {
            n += stage_tree_count(child);
        }
    }
    return n;
}

// [frame,"send",how,stage object,joint,joints]: the stage's own code ends
// generators. how "kept": the placed one its code kept (an "sgen" marked
// "kept"), "kept-now": and its particles at once (grLib_801C9834,
// grLib_801C9874; stage object and joint -1, joints 0). "joints": every
// generator on the joint or under it (grLib_801C9854); "joint-now": on the
// joint alone, particles at once (grLib_801C98A0); "joints-now": the tree,
// particles at once (grLib_801C9908). joints: how many the walk covers, from
// `joint` on in the stage object's preorder.
void msl_viewer_track_send(const char* how, HSD_JObj* jobj, int tree)
{
    int map = -1;
    int joint = -1;
    if (track_out == NULL || msl_viewer_track_plain()) {
        return;
    }
    stage_joint(jobj, &map, &joint);
    fprintf(track_out, "[%d,\"send\",\"%s\",%d,%d,%d]\n", hook_frame(), how,
            map, joint, jobj == NULL ? 0 : tree ? stage_tree_count(jobj) : 1);
}

void msl_viewer_track_sgen(int bank, int gen_id, const Vec3* pos,
                           HSD_JObj* jobj)
{
    msl_viewer_track_sgen_as(bank, gen_id, pos, jobj, NULL);
}

// mark: "on" - made on the joint and going with it (grLib_801C9808), "kept" -
// placed, and kept by the stage's code to end later; none for the rest.
void msl_viewer_track_sgen_as(int bank, int gen_id, const Vec3* pos,
                              HSD_JObj* jobj, const char* mark)
{
    Vec3 at = { 0.0F, 0.0F, 0.0F };

    if (track_out == NULL) {
        return;
    }
    if (pos != NULL) {
        at = *pos;
    } else if (jobj != NULL) {
        at.x = jobj->mtx[0][3];
        at.y = jobj->mtx[1][3];
        at.z = jobj->mtx[2][3];
    }
    if (msl_viewer_track_plain()) {
        fprintf(track_out, "[%d,\"sgen\",%d,%d,%d,%.6g,%.6g,%.6g,%u]\n",
                hook_frame(), running_map_id(), bank, gen_id, at.x, at.y, at.z,
                (unsigned) seed_now());
    } else {
        // (and the joint it is placed from, when it is: -1, -1 at a place)
        int map;
        int joint;
        stage_joint(pos == NULL ? jobj : NULL, &map, &joint);
        if (mark != NULL) {
            fprintf(track_out,
                    "[%d,\"sgen\",%d,%d,%d,%.6g,%.6g,%.6g,%u,%d,%d,\"%s\"]\n",
                    hook_frame(), running_map_id(), bank, gen_id, at.x, at.y,
                    at.z, (unsigned) seed_now(), map, joint, mark);
        } else {
            fprintf(track_out,
                    "[%d,\"sgen\",%d,%d,%d,%.6g,%.6g,%.6g,%u,%d,%d]\n",
                    hook_frame(), running_map_id(), bank, gen_id, at.x, at.y,
                    at.z, (unsigned) seed_now(), map, joint);
        }
    }
    msl_viewer_track_skipped(bank, gen_id);
}

void msl_viewer_track_tex(HSD_GObj* fighter, int slot, float index)
{
    Fighter* fp;

    if (track_out == NULL || fighter == NULL) {
        return;
    }
    fp = GET_FIGHTER(fighter);
    fprintf(track_out, "[%d,\"tex\",%d,%d,%d,%.6g]\n", hook_frame(),
            (int) fp->player_id, fp->x221F_b4 ? 1 : 0, slot, index);
}

static void track_overlay(int frame, int port, int sub, int slot,
                          const ColorOverlay* co)
{
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];
    int index = co->x28_colanim.i;
    uint8_t flags;

    snprintf(key, sizeof(key), "\"tint\",%d,%d,%d", port, sub, slot);
    if (index == 0) {
        const char* last = last_state(key);
        if (last == NULL) {
            return;
        }
        emit_state(frame, key, ",0");
        return;
    }
    // The x7C byte, most significant bit first as declared.
    flags = (uint8_t) ((co->x7C_color_enable << 7) | (co->x7C_flag2 << 6) |
                       (co->x7C_light_enable << 5) | (co->x7C_flag4 << 4) |
                       (co->x7C_flag5 << 3) | (co->x7C_flag6 << 2) |
                       (co->x7C_flag7 << 1) | co->x7C_flag8);
    snprintf(value, sizeof(value), ",%d,%u,%u,%u,%.6g,%.6g", index,
             (unsigned) rgba(co->x2C_hex), (unsigned) flags,
             (unsigned) rgba(co->x50_light_color), co->x74_light_rot_x,
             co->x78_light_rot_yz);
    emit_state(frame, key, value);
}

static void track_charge(int frame, int port, int sub, const char* field,
                         long value)
{
    char key[TRACK_KEY_LEN];
    char text[32];
    snprintf(key, sizeof(key), "\"charge\",%d,%d,\"%s\"", port, sub, field);
    snprintf(text, sizeof(text), ",%ld", value);
    emit_state(frame, key, text);
}

// The draw offset ftCommon_8008021C stores for ftLib_80087074 (hit shake,
// the x2224_b5 wobble, and the smash-charge shake). The hosted build skips
// that render-side function, so the same sum is taken here into locals.
static bool track_shake(Fighter* fp, Vec3* out)
{
    Vec2 shift;
    float x = 0.0F;
    float y = 0.0F;

    if (Fighter_804D6530 != NULL && ftCo_80090690(fp, &shift) != NULL) {
        x += shift.x;
        y += shift.y;
    }
    if (fp->x2224_b5 && Fighter_804D652C != NULL) {
        x += Fighter_804D652C->x0[fp->x1A52].x;
        y += Fighter_804D652C->x0[fp->x1A52].y;
    }
    if (Fighter_804D6528 != NULL && ftCo_800DEEE8(fp, &shift) != NULL) {
        x += shift.x;
        y += shift.y;
    }
    out->x = x;
    out->y = y;
    out->z = 0.0F;
    return x != 0.0F || y != 0.0F;
}

static void track_fighter(int frame, Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    int port = fp->player_id;
    int sub = fp->x221F_b4 ? 1 : 0;
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];
    Vec3 shake;
    int i;

    track_overlay(frame, port, sub, 0, &fp->x408);
    track_overlay(frame, port, sub, 1, &fp->x488);
    track_overlay(frame, port, sub, 2, &fp->x508);

    switch (fp->kind) {
    case FTKIND_DONKEY:
        track_charge(frame, port, sub, "dk", fp->fv.dk.x222C);
        break;
    case FTKIND_SAMUS:
        track_charge(frame, port, sub, "ss", fp->fv.ss.x2230);
        break;
    case FTKIND_MEWTWO:
        track_charge(frame, port, sub, "mt",
                     fp->fv.mt.x2234_shadowBallCharge);
        break;
    case FTKIND_SEAK:
        track_charge(frame, port, sub, "sk", fp->fv.sk.x0);
        break;
    case FTKIND_GAMEWATCH:
        track_charge(frame, port, sub, "gw", fp->fv.gw.x2238_panicCharge);
        break;
    case FTKIND_KIRBY:
        track_charge(frame, port, sub, "kb.kind", (long) fp->fv.kb.hat.kind);
        track_charge(frame, port, sub, "kb.dk", fp->fv.kb.xBC);
        track_charge(frame, port, sub, "kb.ss", fp->fv.kb.xA8);
        break;
    default:
        break;
    }

    // Model part variants as shown (ftParts_80074A4C followed by the apply,
    // or the script's ftParts_80074B0C).
    for (i = 0; i < fp->x5AC.model_num && i < 12; ++i) {
        snprintf(key, sizeof(key), "\"part\",%d,%d,%d", port, sub, i);
        snprintf(value, sizeof(value), ",%d", (int) fp->x5F4_arr[i].idx);
        emit_state(frame, key, value);
    }

    // The animation as the game plays it: its index in the subaction table,
    // whose table (the fighter's own kind, or a thrower's), the frame, the
    // rate, and the blend into it (blend frames, and how far in: ftAnim_8006EBA4
    // ftAnim_8006FE9C).
    snprintf(key, sizeof(key), "\"anim\",%d,%d", port, sub);
    snprintf(value, sizeof(value), ",%d,%d,%.6g,%.6g,%.6g,%.6g",
             (int) fp->anim_id, (int) Fighter_AnimSourceKind(fp),
             fp->cur_anim_frame, fp->frame_speed_mul,
             fp->x8A4_animBlendFrames, fp->x8A8_anim_frame);
    emit_state(frame, key, value);

    if (world_out != NULL && fp->parts != NULL) {
        int16_t head[4];
        int n = (int) ftPartsTable[fp->kind]->parts_num;
        head[0] = (int16_t) frame;
        head[1] = (int16_t) port;
        head[2] = (int16_t) sub;
        head[3] = (int16_t) n;
        fwrite(head, sizeof(head), 1, world_out);
        for (i = 0; i < n; ++i) {
            uint32_t flags = 0;
            float v[10] = { 0 };
            HSD_JObj* j = fp->parts[i].joint;
            if (j != NULL) {
                flags = j->flags;
                v[0] = j->rotate.x; v[1] = j->rotate.y; v[2] = j->rotate.z;
                v[3] = j->rotate.w; v[4] = j->scale.x; v[5] = j->scale.y;
                v[6] = j->scale.z; v[7] = j->translate.x;
                v[8] = j->translate.y; v[9] = j->translate.z;
            }
            fwrite(&flags, sizeof(flags), 1, world_out);
            fwrite(v, sizeof(v), 1, world_out);
        }
    }

    // DeadUpStar/DeadUpFall (Slippi action states 4-10) or off the plane:
    // the top joint's place, turn and size; and once more the frame it is
    // back (the state over, or on the plane again), so a reader sees it come
    // back.
    {
        int off = (fp->motion_id >= 4 && fp->motion_id <= 10) ||
                  fp->cur_pos.z != 0.0F;
        int at = port * 2 + sub;
        int was = at >= 0 && at < 12 ? track_record.pos_off[at] : 0;
        HSD_JObj* top = fp->parts != NULL ? fp->parts[FtPart_TopN].joint
                                          : NULL;
        if (at >= 0 && at < 12) {
            track_record.pos_off[at] = (unsigned char) off;
        }
        if (top != NULL && (off || was)) {
            snprintf(key, sizeof(key), "\"pos\",%d,%d", port, sub);
            snprintf(value, sizeof(value),
                     ",%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g", top->translate.x,
                     top->translate.y, top->translate.z, top->rotate.x,
                     top->rotate.y, top->rotate.z, top->scale.x);
            emit_state(frame, key, value);
        }
    }

    snprintf(key, sizeof(key), "\"shake\",%d,%d", port, sub);
    if (track_shake(fp, &shake)) {
        snprintf(value, sizeof(value), ",%.6g,%.6g,%.6g", shake.x, shake.y,
                 shake.z);
        emit_state(frame, key, value);
    } else if (last_state(key) != NULL) {
        emit_state(frame, key, ",0,0,0");
    }

    snprintf(key, sizeof(key), "\"hide\",%d,%d", port, sub);
    snprintf(value, sizeof(value), ",%d",
             (fp->invisible ? 1 : 0) | (fp->x221E_b5 ? 2 : 0) |
                 (fp->x2226_b5 ? 4 : 0));
    if (strcmp(value, ",0") != 0 || last_state(key) != NULL) {
        emit_state(frame, key, value);
    }

    // Its XRotN hung on its captor's TransN2 (ftCo_800DB368's position
    // constraint, x2226_b2), from when the pull brings it in until the
    // throw lets go (ftCo_800DDDE4) or the grab breaks (ftCo_CaptureCut):
    // the fighter's first frame and every change, so a reader knows the
    // track has it.
    snprintf(key, sizeof(key), "\"cns\",%d,%d", port, sub);
    snprintf(value, sizeof(value), ",%d", fp->x2226_b2 ? 1 : 0);
    emit_state(frame, key, value);
}

static float round2(float v)
{
    return roundf(v * 100.0F) / 100.0F;
}

static void track_stage(int frame, const MslCoreMatch* match)
{
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];
    Vec3 eye;
    Vec3 interest;
    float fov;
    Vec2 translation;
    u8 bg[3];
    int i;

    // Battlefield / Final Destination map part 3.
    if (match->config.stage_id == TRACK_STAGE_BATTLEFIELD ||
        match->config.stage_id == TRACK_STAGE_FINAL_DESTINATION)
    {
        HSD_GObj* gobj = Ground_801C2BA4(3);
        if (gobj != NULL) {
            Ground* gp = GET_GROUND(gobj);
            if (match->config.stage_id == TRACK_STAGE_BATTLEFIELD) {
                snprintf(value, sizeof(value), ",%d,%d,%d",
                         gp->u.battle.bg_state, gp->u.battle.curr_bg,
                         gp->u.battle.prev_bg);
                emit_state(frame, "\"bg\"", value);
            } else {
                float frames = gp->u.map.xC4_b27 ? 0.0F : gp->u.map.x104;
                snprintf(value, sizeof(value), ",%d,%.6g,%.6g,%.6g,%.6g",
                         (int) gp->u.map.xC4_b2_25, gp->u.map.xE4,
                         gp->u.map.xE8, gp->u.map.xEC,
                         frames > 0.0F ? frames : 0.0F);
                emit_state(frame, "\"fd\"", value);
                // Part 7's root turns while the background is in phases 9
                // and 10 (grLast_8021C40C and its animation).
                if (gp->u.map.xC4_b2_25 == 9 || gp->u.map.xC4_b2_25 == 10) {
                    HSD_GObj* part = Ground_801C2BA4(7);
                    HSD_JObj* root = part != NULL ? part->hsd_obj : NULL;
                    if (root != NULL) {
                        snprintf(value, sizeof(value), ",%.6g,%.6g,%.6g",
                                 root->rotate.x, root->rotate.y,
                                 root->rotate.z);
                        emit_state(frame, "\"fdrot\"", value);
                    }
                }
            }
        }
    }

    // Fountain of Dreams: how high each platform stands, left then right -
    // each water column's own height (grIzumi_801CC358's xD0: the file's
    // units, what its joint is stretched by and the platform's joint set to).
    // A replay says it from Slippi 3.18 on; the game's own is here for all.
    // refs/melee/src/melee/gr/grizumi.c::{grIzumi_801CCBDC,grIzumi_801CC358}
    if (match->config.stage_id == TRACK_STAGE_FOUNTAIN && match->gobj.entities != NULL &&
        !msl_viewer_track_plain())
    {
        float h[2] = { 0.0F, 0.0F };
        int have = 0;
        HSD_GObj* gobj;
        for (gobj = (HSD_GObj*) match->gobj.entities->x14; gobj != NULL; gobj = gobj->next) {
            Ground* gp;
            if (gobj->classifier != HSD_GOBJ_CLASS_STAGE || gobj->user_data == NULL) {
                continue;
            }
            gp = GET_GROUND(gobj);
            if ((int) gp->map_id == 4 && (gp->gv.izumi3.xC8 == 0 || gp->gv.izumi3.xC8 == 1)) {
                h[gp->gv.izumi3.xC8] = gp->gv.izumi3.xD0;
                have |= 1 << gp->gv.izumi3.xC8;
            }
        }
        if (have == 3) {
            snprintf(value, sizeof(value), ",%.6g,%.6g", h[0], h[1]);
            emit_state(frame, "\"fod\"", value);
        }
    }

    // Stage material colour scripts (grMaterial_801C9604) while they run.
    for (i = 0; i < 64; ++i) {
        HSD_GObj* gobj = Ground_801C2BA4(i);
        Ground* gp;
        uint32_t colour = 0;
        if (gobj == NULL) {
            continue;
        }
        gp = GET_GROUND(gobj);
        if (gp->x10_flags.b4 && gp->color_overlay.x7C_color_enable) {
            ColorOverlay* co = &gp->color_overlay;
            GXColor c;
            c.r = (u8) co->x30_color_red;
            c.g = (u8) co->x34_color_green;
            c.b = (u8) co->x38_color_blue;
            c.a = (u8) co->x3C_color_alpha;
            colour = rgba(c);
        }
        snprintf(key, sizeof(key), "\"gcolor\",%d", (int) gp->map_id);
        snprintf(value, sizeof(value), ",%u", (unsigned) colour);
        if (colour != 0 || last_state(key) != NULL) {
            emit_state(frame, key, value);
        }
    }

    msl_camera_get_render_transform(&eye, &interest, &fov);
    msl_camera_track_extras(&translation, bg);
    snprintf(value, sizeof(value), ",%u",
             ((unsigned) bg[0] << 24) | ((unsigned) bg[1] << 16) |
                 ((unsigned) bg[2] << 8) | 0xFFU);
    emit_state(frame, "\"bgcolor\"", value);
    // The render transform includes the quake translation; report the
    // unshaken position and the translation separately.
    snprintf(value, sizeof(value),
             ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
             round2(eye.x - translation.x), round2(eye.y - translation.y),
             round2(eye.z), round2(interest.x - translation.x),
             round2(interest.y - translation.y), round2(interest.z),
             round2(fov), round2(translation.x), round2(translation.y));
    emit_state(frame, "\"cam\"", value);
}

// Owner port and sub (0 leader, 1 Nana) of an item whose owner is a fighter.
static bool item_owner_port(Item* ip, int* port, int* sub)
{
    if (ip->owner == NULL || ip->owner->classifier != HSD_GOBJ_CLASS_FIGHTER) {
        return false;
    }
    *port = GET_FIGHTER(ip->owner)->player_id;
    *sub = GET_FIGHTER(ip->owner)->x221F_b4 ? 1 : 0;
    return true;
}

// A chain as [x, y] pairs: `head` first when given, then each link from
// `start` along next (`along_next`) or prev, then `tail` when given. Points
// still at the origin (not out yet) are left out, and with `by_out` each
// link that is not out (x2C_b0 clear: not drawn, it_802A24A0) too; a chain
// with none left writes no event.
static void track_chain(int frame, Item* ip, const Vec3* head,
                        ItemLink* start, bool along_next, const Vec3* tail,
                        bool by_out)
{
    enum { MAX_LINKS = 64 };
    char key[TRACK_KEY_LEN];
    char value[64 * 2 * 12 + 64];
    size_t used = 0;
    int port;
    int sub;
    int count = 0;
    ItemLink* link;

    if (!item_owner_port(ip, &port, &sub)) {
        port = -1;
        sub = 0;
    }
    if (head != NULL && (head->x != 0.0F || head->y != 0.0F)) {
        used += (size_t) snprintf(value + used, sizeof(value) - used,
                                  ",%.2f,%.2f", head->x, head->y);
    }
    for (link = start; link != NULL && count < MAX_LINKS;
         link = along_next ? link->next : link->prev, ++count)
    {
        if (&link->pos == head || &link->pos == tail ||
            (link->pos.x == 0.0F && link->pos.y == 0.0F) ||
            (by_out && !link->x2C_b0))
        {
            continue;
        }
        used += (size_t) snprintf(value + used, sizeof(value) - used,
                                  ",%.2f,%.2f", link->pos.x, link->pos.y);
        if (used >= sizeof(value) - 64) {
            break;
        }
    }
    if (tail != NULL && (tail->x != 0.0F || tail->y != 0.0F)) {
        used += (size_t) snprintf(value + used, sizeof(value) - used,
                                  ",%.2f,%.2f", tail->x, tail->y);
    }
    if (used == 0) {
        return;
    }
    snprintf(key, sizeof(key), "\"hook\",%d,%d", port, sub);
    fprintf(track_out, "[%d,%s%s]\n", frame, key, value);
}

static void track_items(int frame, const MslCoreMatch* match)
{
    Item_GObj* gobj = (Item_GObj*) match->gobj.entities->items;
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];

    while (gobj != NULL) {
        Item* ip = GET_ITEM(gobj);
        HSD_JObj* jobj = GET_JOBJ(gobj);

        switch (ip->kind) {
        case It_Kind_Link_HShot:
        case It_Kind_CLink_HShot: {
            // The hook is the last link (xDD4+0); the item itself stays at
            // its spawn point. Head first, then back toward the hand.
            ItemLink* hook = ip->xDD4_itemVar.linkhookshot.x0;
            if (hook != NULL) {
                track_chain(frame, ip, &hook->pos, hook, true, NULL,
                            false);
            }
            break;
        }
        case It_Kind_Samus_GBeam: {
            // The claw is the last link (xDD4+0), which the grapple moves and
            // collides; the other links follow joint animation the headless
            // core does not run, so only the ones that have moved appear.
            ItemLink* claw = ip->xDD4_itemVar.samusgrapple.x0;
            if (claw != NULL) {
                track_chain(frame, ip, &claw->pos, claw, true, NULL,
                            false);
            }
            break;
        }
        case It_Kind_Ness_Yoyo: {
            // Ness's yo-yo: the string's links from the hand (xDD4+C) toward
            // the ball, then the ball (xDD4+8, the link the yo-yo's hitbox
            // position is written to) last. The item itself stays at its
            // spawn point. A link not out keeps its spawn place or the hand's
            // (it_802BF28C, it_802BF4A0 leave it there), never the origin,
            // so it is told by x2C_b0: set as the string pays out from the
            // ball (it_802BF28C, it_802BF4A0) and for the ball itself
            // (it_802BE65C, it_802C0010), cleared as it is drawn back
            // (it_802BF800) or under the shortest length (it_802BF28C), so
            // the links out are one run from the ball. ("lm" writes the same
            // bit as each link's out.)
            ItemLink* ball = ip->xDD4_itemVar.nessyoyo.x8;
            track_chain(frame, ip, NULL, ip->xDD4_itemVar.nessyoyo.xC,
                        false, ball != NULL ? &ball->pos : NULL, true);
            break;
        }
        default:
            break;
        }
        // Which of its owner's articles (ftData x48_items, the list each
        // fighter's OnLoad registers its items' kinds from: it_8026B3F8)
        // this kind of item is drawn with, once per kind: [frame,"art",kind,
        // owner's character, index], -1 when it is none of them (a Kirby
        // copy's, an item of the stage's).
        snprintf(key, sizeof(key), "\"art\",%d", (int) ip->kind);
        if (last_state(key) == NULL) {
            int found = -1;
            int character = -1;
            if (ip->owner != NULL &&
                ip->owner->classifier == HSD_GOBJ_CLASS_FIGHTER)
            {
                Fighter* fp = GET_FIGHTER(ip->owner);
                Article** items = fp->ft_data != NULL
                                      ? (Article**) fp->ft_data->x48_items
                                      : NULL;
                int k;
                character = (int) fp->kind;
                for (k = 0; items != NULL && k < 16; ++k) {
                    if (items[k] == ip->xC4_article_data) {
                        found = k;
                        break;
                    }
                }
            }
            snprintf(value, sizeof(value), ",%d,%d", character, found);
            emit_state(frame, key, value);
        }
        // The root joint's Euler rotation, on change (spinning throws,
        // turnips, bombs).
        if (jobj != NULL && !(jobj->flags & JOBJ_USE_QUATERNION)) {
            snprintf(key, sizeof(key), "\"irot\",%d", (int) ip->x1C);
            snprintf(value, sizeof(value), ",%.4f,%.4f,%.4f", jobj->rotate.x,
                     jobj->rotate.y, jobj->rotate.z);
            if (last_state(key) != NULL || jobj->rotate.x != 0.0F ||
                jobj->rotate.y != 0.0F || jobj->rotate.z != 0.0F)
            {
                emit_state(frame, key, value);
            }
        }
        gobj = (Item_GObj*) gobj->next;
    }
}

// The HUD's stock icons: a lost stock's burst. The simulator has no HUD, so
// its part of the game's frame is kept here for the track: each player's
// icons' counts as ifStock_802F8298 steps them (an icon the player still has
// stands at 0; one just lost is found at 0, bursts - efSync_Spawn 0x474, the
// common file's generator 0xF7 on particle list 1 at the icon's place - and
// counts up to 10). The HUD's proc runs at priority 17, after the particles'
// update (15): the generator is made after the frame's update and takes its
// first step in the next frame's, where it stands before the KO blast's own
// in the list. Seen in the game's memory (a frame-locked Dolphin read).
// The places are the HUD file's own (IfAll: ScInfDmg's joints 2 to 5, a slot
// a port; Stc_scemdls' joints 1 to 5 as their animation's frame 0 leaves
// them, the icons). Not here: a stolen stock's
// two (0x475, 0x476: teams), and the donor's icon then standing still.
// [frame,"hgen",bank,id,x,y,z,seed,port,icon]
// refs/melee/src/melee/if/ifstock.c::{ifStock_802F8298,ifStock_802F98E8}
static struct {
    unsigned char seen;
    unsigned char anim[5];
} track_hud[6];

static void track_hud_reset(void)
{
    memset(track_hud, 0, sizeof(track_hud));
}

static void track_hud_step(int frame, const MslCoreMatch* match)
{
    static const float slot_x[4] = { -21.0F, -7.0F, 7.0F, 21.0F };
    static const float icon_x[5] = { -4.999755859375F, -2.5999755859375F,
                                     -0.199951171875F, 2.199951171875F,
                                     4.599853515625F };
    int i;

    for (i = 0; i < match->config.num_players; ++i) {
        int port;
        int stocks;
        int k;
        if (match->fighters[i] == NULL) {
            continue;
        }
        port = GET_FIGHTER(match->fighters[i])->player_id;
        if (port < 0 || port >= 4) {
            continue;
        }
        stocks = Player_GetStocks(port);
        if (stocks > 99) {
            stocks = 99;
        }
        if (stocks < 0) {
            stocks = 1;
        }
        if (!track_hud[port].seen) {
            // (as the icons are made: ifStock_802F98E8)
            track_hud[port].seen = 1;
            for (k = 0; k < 5; ++k) {
                track_hud[port].anim[k] = stocks <= 5 && k >= stocks ? 10 : 0;
            }
        }
        if (stocks > 5) {
            track_hud[port].anim[0] = 0;
            for (k = 1; k < 5; ++k) {
                track_hud[port].anim[k] = 10;
            }
            continue;
        }
        for (k = 0; k < 5; ++k) {
            unsigned char* anim = &track_hud[port].anim[k];
            if (k < stocks) {
                *anim = 0;
                continue;
            }
            if (*anim == 0) {
                Vec3 pos;
                Vec3 none = { 0.0F, 0.0F, 0.0F };
                Vec3 one = { 1.0F, 1.0F, 1.0F };
                u32 state = seed_now();
                void* made;
                pos.x = icon_x[k] + slot_x[port];
                pos.y = 7.5F + -21.0F;
                pos.z = 0.0F;
                // (0xF7 counts nothing at random as it is made: the state is
                // written for the form's sake)
                fprintf(track_out,
                        "[%d,\"hgen\",0,%d,%.9g,%.9g,%.9g,%u,%d,%d]\n", frame,
                        0xF7, pos.x, pos.y, pos.z, (unsigned) state, port, k);
                made = msl_particle_probe_generator(0xF7, NULL, MSL_PROBE_PLACE, 1, state);
                msl_particle_probe_placed(made, frame, 0xF7, MSL_PROBE_PLACE, state, &pos, 0, &none, &none, &one);
            }
            if (*anim < 10) {
                ++*anim;
            }
        }
    }
}

// Poses: the joints whose pose the game's own code sets apart from an
// animation a viewer plays itself, as that code left them at the frame's end,
// so a viewer can draw them as the game does. Each is written only when it
// changes ("mask" bit k says which values follow, in order).
//
//   [frame,"jp",item,(joint,mask,values)...]
//       an item's joints below its root (the root is the item's place and
//       "irot"), by preorder index in its model as the file lays it out (an
//       instance's children counted): rot x, y, z, scale x, y, z, translate
//       x, y, z (mask bits 0-8). A joint is written once it differs from the
//       file's rest pose, and then whenever it changes.
//   [frame,"fp",port,sub,character,(part,mask,values)...]
//       a fighter's swinging joints (ftdynamics.c): the chains a hurtbox sits
//       on, which gameplay solves (Fox's tail, Mewtwo's, Jigglypuff's), and
//       with the track the capes, caps and hair the gameplay build drops
//       (msl_viewer_track_keep_dynamics), by part, the same nine values. A
//       fighter's first line, and its first as another character, has every
//       value.
//   [frame,"fj",port,sub,character,(part,mask,values)...]
//       a fighter's joints as the game left them at the frame's end, by
//       part, the same nine values: its animation as ftAnim_8006EBA4 and
//       the interpreter played it, the blend into it (ftAnim_8006FE9C,
//       ftAnim_80070010), and whatever the game's own code set after
//       (ftParts, the state's code, the swinging chains), so a viewer need
//       not work any of it out. Every part with a joint of its own that
//       gameplay keeps live: the presentation-only parts the hosted build
//       leaves cold (ftParts_HeadlessGameplayMask: fingers, faces) are not
//       animated there and are not written; of part 0 (the fighter's
//       root) only its turn and size are (values 0 to 5: its facing, the
//       tilts and spins the state's code sets, its model scale), its place
//       being "pos"'s and the replay's; nor is a joint
//       turned by a quaternion, nor a swinging chain's joints when the
//       track build does not solve that chain (its links pruned, no copy
//       kept: msl_viewer_track_keep_dynamics), which stand unsolved here and
//       are the viewer's to solve. Each value only as it changes; a fighter's
//       first line, and its first as another character, has every value.
//   [frame,"lm",item,links,(link,out,mask,values)...]
//       a chain's links (Link's and Young Link's hookshots, Samus's grapple,
//       Sheik's chain, Ness's yo-yo, the Ice Climbers' belay): each link's
//       matrix as the item's code set it (it_802A6DC8, it_802A6F80: its turn
//       along the chain, its place and size), rows 0-2 of the 3x4 (mask bits
//       0-11).
//       Link 0 is the end with no prev, `links` the chain's length; out is
//       the link's x2C_b0 (it_802A24A0 draws a link only then). A link
//       coming out or going back is written whole.
//   [frame,"sp",part,(joint,mask,values)...]
//       a stage part's joints (the moving parts: Fountain of Dreams'
//       platforms, Yoshi's Story's, Pokemon Stadium's, and whatever else its
//       animations and code move), by the part's number as "gjoint" counts
//       it (its map_id, an extra copy's map_id + 1000 * k) and the joint's
//       preorder index in the part's tree as stage_joint counts it (0 the
//       part's root - Ground_801C14D0's scale joint, at rest rot 0, scale
//       the stage's (its parameters' x0, else 1), translate 0 - then the
//       file's tree from 1, an instance's children left out), the same nine
//       values as "jp". A joint is written once it differs from the file's
//       rest pose, and then whenever it changes. A part the stage file has
//       no joint tree for is not written.
// refs/melee/src/melee/{ft/ftdynamics.c,it/items/itlinkhookshot.c,
//   it/items/itseakchain.c,it/items/itnessyoyo.c,
//   it/items/itclimbersstring.c,gr/ground.c::Ground_801C14D0}

// The owner of a pose, kept while it is there.
static MslTrackPose* owner_in(MslTrackPose* pool, int count, char kind,
                              int id, int frame)
{
    int i;
    int free_at = -1;
    for (i = 0; i < count; ++i) {
        MslTrackPose* o = &pool[i];
        if (o->used && o->kind == kind && o->id == id) {
            o->frame = frame;
            return o;
        }
        if (!o->used && free_at < 0) {
            free_at = i;
        }
    }
    if (free_at < 0) {
        return NULL;
    }
    {
        MslTrackPose* o = &pool[free_at];
        memset(o, 0, sizeof(*o));
        o->used = 1;
        o->kind = kind;
        o->id = id;
        o->frame = frame;
        return o;
    }
}

static MslTrackPose* pose_owner(MslViewerTrack* track, char kind, int id,
                                int frame)
{
    return owner_in(track->pose, TRACK_POSE_OWNERS, kind, id, frame);
}

static void pose_append(MslViewerTrack* track, const char* format, ...)
{
    size_t room = sizeof(track->pose_line) - track->pose_used;
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(track->pose_line + track->pose_used, room, format, args);
    va_end(args);
    if (n < 0 || (size_t) n >= room) {
        fprintf(stderr, "viewer track: pose line too long\n");
        abort();
    }
    track->pose_used += (size_t) n;
}

// One joint against its last written values: ",joint[extra],mask,v..." when
// any differs bit for bit (or none was written), and they are kept.
static void pose_joint(MslViewerTrack* track, MslTrackPose* o, int joint,
                       int extra, const float* v, int n)
{
    unsigned mask = 0;
    int k;
    if (joint < 0 || joint >= TRACK_POSE_JOINTS) {
        return;
    }
    for (k = 0; k < n; ++k) {
        if (!o->known[joint] || vt_f32(v[k]) != vt_f32(o->last[joint][k])) {
            mask |= 1U << k;
        }
    }
    if (mask == 0) {
        return;
    }
    if (extra >= 0) {
        pose_append(track, ",%d,%d,%u", joint, extra, mask);
    } else {
        pose_append(track, ",%d,%u", joint, mask);
    }
    for (k = 0; k < n; ++k) {
        if (mask & (1U << k)) {
            pose_append(track, ",\"0x%08x\"", vt_f32(v[k]));
        }
        o->last[joint][k] = v[k];
    }
    o->known[joint] = 1;
}

// The owner's line, when it has any joint.
static void pose_flush(MslViewerTrack* track, const char* head)
{
    if (track->pose_used > 0) {
        put(track, "%s%s]\n", head, track->pose_line);
    }
    track->pose_used = 0;
    track->pose_line[0] = '\0';
}

static void joint_values(const HSD_JObj* j, float v[9])
{
    v[0] = j->rotate.x;
    v[1] = j->rotate.y;
    v[2] = j->rotate.z;
    v[3] = j->scale.x;
    v[4] = j->scale.y;
    v[5] = j->scale.z;
    v[6] = j->translate.x;
    v[7] = j->translate.y;
    v[8] = j->translate.z;
}

static void pose_rest(MslTrackPose* o, int at, const HSD_Joint* d)
{
    o->last[at][0] = d->rotation.x;
    o->last[at][1] = d->rotation.y;
    o->last[at][2] = d->rotation.z;
    o->last[at][3] = d->scale.x;
    o->last[at][4] = d->scale.y;
    o->last[at][5] = d->scale.z;
    o->last[at][6] = d->position.x;
    o->last[at][7] = d->position.y;
    o->last[at][8] = d->position.z;
    o->known[at] = 1;
}

// An item's model and its file's joints together, in the file's preorder.
// A joint the file has and the model does not (an instance's children) is
// counted and left out.
static void pose_item_walk(MslViewerTrack* track, MslTrackPose* o,
                           HSD_JObj* j, HSD_Joint* d, int* index)
{
    for (; d != NULL; d = d->next, j = j != NULL ? j->next : NULL) {
        int at = (*index)++;
        if (at > 0 && at < TRACK_POSE_JOINTS && j != NULL &&
            !(j->flags & JOBJ_USE_QUATERNION))
        {
            float v[9];
            if (!o->known[at]) {
                // (the file's rest pose: what a viewer has without a line)
                pose_rest(o, at, d);
            }
            joint_values(j, v);
            pose_joint(track, o, at, -1, v, 9);
        }
        if (d->child != NULL) {
            pose_item_walk(track, o,
                           j != NULL && !(j->flags & JOBJ_INSTANCE) ? j->child
                                                                    : NULL,
                           d->child, index);
        }
    }
}

// A chain's links, from the end with no prev along next.
static void pose_links(MslViewerTrack* track, int frame, Item* ip,
                       ItemLink* any)
{
    MslTrackPose* o;
    ItemLink* link = any;
    char head[64];
    int n;
    if (link == NULL) {
        return;
    }
    for (n = 0; link->prev != NULL && n < TRACK_POSE_JOINTS; ++n) {
        link = link->prev;
    }
    o = pose_owner(track, 'l', (int) ip->x1C, frame);
    if (o == NULL) {
        return;
    }
    for (n = 0; link != NULL && n < TRACK_POSE_JOINTS; link = link->next, ++n)
    {
        HSD_JObj* j = link->gobj != NULL ? link->gobj->hsd_obj : NULL;
        float v[12];
        int r;
        if (j == NULL) {
            continue;
        }
        for (r = 0; r < 3; ++r) {
            v[r * 4 + 0] = j->mtx[r][0];
            v[r * 4 + 1] = j->mtx[r][1];
            v[r * 4 + 2] = j->mtx[r][2];
            v[r * 4 + 3] = j->mtx[r][3];
        }
        if (o->known[n] && o->flag[n] != link->x2C_b0) {
            o->known[n] = 0;
        }
        o->flag[n] = link->x2C_b0;
        pose_joint(track, o, n, link->x2C_b0 ? 1 : 0, v, 12);
    }
    snprintf(head, sizeof(head), "[%d,\"lm\",%d,%d", frame, (int) ip->x1C,
             n);
    pose_flush(track, head);
}

static void track_item_poses(MslViewerTrack* track, int frame,
                             const MslCoreMatch* match)
{
    Item_GObj* gobj = (Item_GObj*) match->gobj.entities->items;
    char head[64];
    for (; gobj != NULL; gobj = (Item_GObj*) gobj->next) {
        Item* ip = GET_ITEM(gobj);
        HSD_JObj* root = GET_JOBJ(gobj);
        if (root != NULL && ip->xC8_joint != NULL) {
            MslTrackPose* o = pose_owner(track, 'i', (int) ip->x1C, frame);
            int index = 0;
            if (o != NULL) {
                pose_item_walk(track, o, root, ip->xC8_joint, &index);
                snprintf(head, sizeof(head), "[%d,\"jp\",%d", frame,
                         (int) ip->x1C);
                pose_flush(track, head);
            }
        }
        switch (ip->kind) {
        case It_Kind_Link_HShot:
        case It_Kind_CLink_HShot:
            pose_links(track, frame, ip, ip->xDD4_itemVar.linkhookshot.x0);
            break;
        case It_Kind_Samus_GBeam:
            pose_links(track, frame, ip, ip->xDD4_itemVar.samusgrapple.x0);
            break;
        case It_Kind_Seak_Chain:
            pose_links(track, frame, ip, ip->xDD4_itemVar.seakchain.x4);
            break;
        case It_Kind_Ness_Yoyo:
            pose_links(track, frame, ip, ip->xDD4_itemVar.nessyoyo.x8);
            break;
        case It_Kind_IceClimber_GumStrings:
            pose_links(track, frame, ip, ip->xDD4_itemVar.climbersstring.x8);
            break;
        default:
            break;
        }
    }
}

// Whether the hosted build stopped this stage part's animation
// (configure_headless_ground_schedule: a part outside its stage's
// runtime_animation_mask has its animation removed and the headless proc in
// Ground_801C1CD0's place). Its joints then stand where the animation was
// when it stopped, which is not the game's pose.
static int stage_animation_stopped(const HSD_GObj* gobj)
{
    const HSD_GObjProc* proc;
    for (proc = gobj->proc; proc != NULL; proc = proc->child) {
        if (proc->on_invoke == msl_ground_headless_epoch_proc) {
            return 1;
        }
    }
    return 0;
}

// A stage part's model and its file's joints together, as joint_walk counts
// them (an instance's children left out of both). A joint the file has and
// the model does not, or the other way, ends that branch. A joint is written
// once it leaves its file's rest pose; on a part whose animation the hosted
// build stopped, once it leaves the pose it first had here instead (where
// the stopped animation left it), so a joint only the game's code moves is
// still written and one only its animation moves is left to the viewer.
static void pose_stage_walk(MslViewerTrack* track, MslTrackPose* o,
                            HSD_JObj* j, const HSD_Joint* d, int* index,
                            int stopped)
{
    for (; d != NULL && j != NULL; d = d->next, j = j->next) {
        int at = (*index)++;
        if (at < TRACK_POSE_JOINTS && !(j->flags & JOBJ_USE_QUATERNION)) {
            float v[9];
            joint_values(j, v);
            if (!o->known[at]) {
                if (stopped) {
                    memcpy(o->last[at], v, sizeof(v));
                    o->known[at] = 1;
                } else {
                    pose_rest(o, at, d);
                }
            }
            pose_joint(track, o, at, -1, v, 9);
        }
        if (!(j->flags & JOBJ_INSTANCE)) {
            pose_stage_walk(track, o, j->child, d->child, index, stopped);
        }
    }
}

// Each stage part with a joint tree in the stage's file, numbered as
// stage_joint numbers them.
// refs/melee/src/melee/gr/ground.c::Ground_801C14D0
static void track_stage_poses(MslViewerTrack* track, int frame,
                              const MslCoreMatch* match)
{
    HSD_GObj* gobj;
    int others[64];
    char head[64];
    float scale = stage_info.param != NULL ? stage_info.param->x0 : 1.0F;
    memset(others, 0, sizeof(others));
    for (gobj = (HSD_GObj*) match->gobj.entities->x14; gobj != NULL;
         gobj = gobj->next)
    {
        UnkArchiveStruct* archive;
        const HSD_Joint* desc;
        HSD_JObj* root;
        MslTrackPose* o;
        int map_id;
        int id;
        int index = 1;
        float v[9];
        if (gobj->classifier != HSD_GOBJ_CLASS_STAGE ||
            gobj->user_data == NULL || gobj->hsd_obj == NULL)
        {
            continue;
        }
        map_id = (int) GET_GROUND(gobj)->map_id;
        if (map_id < 0 || map_id >= 64) {
            continue;
        }
        id = map_id;
        if (stage_info.x180[map_id] != gobj) {
            id = map_id + 1000 * ++others[map_id];
        }
        archive = grDatFiles_801C6330(map_id);
        if (archive == NULL || archive->unk4 == NULL ||
            map_id >= archive->unk4->unkC)
        {
            continue;
        }
        desc = archive->unk4->unk8[map_id].unk0;
        root = gobj->hsd_obj;
        if (desc == NULL) {
            continue;
        }
        o = pose_owner(track, 's', id, frame);
        if (o == NULL) {
            continue;
        }
        if (!o->known[0]) {
            memset(o->last[0], 0, sizeof(o->last[0]));
            o->last[0][3] = o->last[0][4] = o->last[0][5] = scale;
            o->known[0] = 1;
        }
        if (!(root->flags & JOBJ_USE_QUATERNION)) {
            joint_values(root, v);
            pose_joint(track, o, 0, -1, v, 9);
        }
        pose_stage_walk(track, o, root->child, desc, &index,
                        stage_animation_stopped(gobj));
        snprintf(head, sizeof(head), "[%d,\"sp\",%d", frame, id);
        pose_flush(track, head);
    }
}

static void track_fighter_poses(MslViewerTrack* track, int frame,
                                Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    int port = fp->player_id;
    int sub = fp->x221F_b4 ? 1 : 0;
    MslTrackPose* o;
    char head[64];
    int i;
    if (fp->parts == NULL || fp->dynamics_num <= 0) {
        return;
    }
    o = pose_owner(track, 'f', port * 2 + sub, frame);
    if (o == NULL) {
        return;
    }
    if (o->character != (int) fp->kind + 1) {
        memset(o->known, 0, sizeof(o->known));
        o->character = (int) fp->kind + 1;
    }
    for (i = 0; i < (int) fp->dynamics_num; ++i) {
        struct DynamicsData* link = fp->dynamic_bone_sets[i].dyn_desc.data;
        for (; link != NULL; link = link->next) {
            HSD_JObj* j = link->desc.lb_unk0.jobj;
            u32 part;
            if (j == NULL || (j->flags & JOBJ_USE_QUATERNION)) {
                continue;
            }
            for (part = 0; part < ftPartsTable[fp->kind]->parts_num; ++part)
            {
                if (fp->parts[part].joint == j) {
                    float v[9];
                    joint_values(j, v);
                    pose_joint(track, o, (int) part, -1, v, 9);
                    break;
                }
            }
        }
    }
    snprintf(head, sizeof(head), "[%d,\"fp\",%d,%d,%d", frame, port, sub,
             (int) fp->kind);
    pose_flush(track, head);
}

// The parts on a swinging chain this build leaves unsolved: the chains whose
// links the prune took (ftCo_HeadlessPruneDynamics with no copy kept), each
// the joints ftCo_8009CF84's lb_8000FD48 links, from its bone down its first
// children.
// refs/melee/src/melee/ft/ftdynamics.c::ftCo_8009CF84,
// refs/melee/src/melee/lb/lbspdisplay.c::lb_8000FD48
static void unsolved_chain_parts(const Fighter* fp, int count, uint8_t* out)
{
    const ftData* data = fp->ft_data;
    int i;
    memset(out, 0, (size_t) count);
    if (data == NULL || data->x2C == NULL || data->x2C->ftDynamicBones == NULL) {
        return;
    }
    for (i = 0; i < data->x2C->dynamicsNum && i < Ft_Dynamics_NumMax; ++i) {
        const BoneDynamicsDesc* bones = &data->x2C->ftDynamicBones->array[i];
        HSD_JObj* j;
        unsigned int link;
        if (fp->dynamic_bone_sets[i].dyn_desc.data != NULL ||
            bones->bone_id < 0 || bones->bone_id >= count)
        {
            continue;
        }
        j = fp->parts[bones->bone_id].joint;
        for (link = 0; j != NULL && link < bones->dyn_desc.count; ++link) {
            int part;
            for (part = 0; part < count; ++part) {
                if (fp->parts[part].joint == j) {
                    out[part] = 1;
                    break;
                }
            }
            j = j->child;
        }
    }
}

// Every live part's joint (the "fj" lines).
// refs/melee/src/melee/ft/{ftanim.c::ftAnim_8006EBA4,ftparts.c}
static void track_fighter_joints(MslViewerTrack* track, int frame,
                                 Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    int port = fp->player_id;
    int sub = fp->x221F_b4 ? 1 : 0;
    MslTrackPose* o;
    char head[64];
    int part;
    int count;
    if (fp->parts == NULL) {
        return;
    }
    o = pose_owner(track, 'j', port * 2 + sub, frame);
    if (o == NULL) {
        return;
    }
    if (o->character != (int) fp->kind + 1) {
        memset(o->known, 0, sizeof(o->known));
        o->character = (int) fp->kind + 1;
    }
    count = (int) ftPartsTable[fp->kind]->parts_num;
    if (count > TRACK_POSE_JOINTS) {
        count = TRACK_POSE_JOINTS;
    }
    unsolved_chain_parts(fp, count, track->chain_parts);
    // (the root: its turn and size, not its place)
    if (count > 0 && fp->parts[0].joint != NULL &&
        !(fp->parts[0].joint->flags & JOBJ_USE_QUATERNION))
    {
        float v[9];
        joint_values(fp->parts[0].joint, v);
        pose_joint(track, o, 0, -1, v, 6);
    }
    for (part = 1; part < count; ++part) {
        HSD_JObj* j = fp->parts[part].joint;
        float v[9];
        if (j == NULL || !fp->parts[part].flags_b1 || track->chain_parts[part] ||
            (j->flags & (JOBJ_MSL_GAMEPLAY_COLD | JOBJ_USE_QUATERNION)))
        {
            continue;
        }
        joint_values(j, v);
        pose_joint(track, o, part, -1, v, 9);
    }
    snprintf(head, sizeof(head), "[%d,\"fj\",%d,%d,%d", frame, port, sub,
             (int) fp->kind);
    pose_flush(track, head);
}

// The chains are copied before the prune (it puts the dropped ones' links
// back on the pool's free list); a copy goes in where the prune left a set
// empty. The pool ends as the prune leaves it, as without the track.
void msl_viewer_track_keep_dynamics(Fighter* fp)
{
    struct DynamicsData* copies[Ft_Dynamics_NumMax];
    int count = fp->dynamics_num < Ft_Dynamics_NumMax ? (int) fp->dynamics_num
                                                    : Ft_Dynamics_NumMax;
    int i;
    for (i = 0; i < count; ++i) {
        copies[i] = msl_viewer_track_copy_dynamics(
            fp->dynamic_bone_sets[i].dyn_desc.data);
    }
    ftCo_HeadlessPruneDynamics(fp);
    for (i = 0; i < count; ++i) {
        DynamicsDesc* desc = &fp->dynamic_bone_sets[i].dyn_desc;
        if (desc->data == NULL && copies[i] != NULL) {
            desc->data = copies[i];
        }
    }
}


// The world matrices ("wf", "wi", "ws"): with MSL_VIEWER_TRACK_WORLD=1, each
// fighter part's, item joint's and stage joint's matrix as the frame's end
// leaves it, the one HSD_JObjMakeMatrix makes from the joint's values (its
// scale compensation, HSD_MtxSRT / HSD_MtxSRTQuat and the concat with its
// parent's, as this build has them), made here into the track's own storage
// so no joint, flag or scale vector of the match is touched. Left out, with
// everything under them: a joint the game's code places another way
// (constraints and limits: an RObj; IK: JOBJ_JOINT1, JOBJ_JOINT2,
// JOBJ_EFFECTOR; JOBJ_USER_DEF_MTX, JOBJ_MTX_INDEP_PARENT, JOBJ_MTX_INDEP_SRT;
// a path animation's translation), a fighter part the hosted build leaves
// cold (no animation there: not the game's pose), and every joint of a
// stage part whose animation the hosted build stops (stage_animation_stopped).
//   [frame,"wf",port,sub,character,(part,mask,values)...]
//   [frame,"wi",item,(joint,mask,values)...]   (joints as "jp", root 0)
//   [frame,"ws",part,(joint,mask,values)...]   (parts and joints as "sp")
// The twelve values are rows 0 to 2 of the 3 x 4 matrix, row by row; mask
// bit k says value k follows; a value is written when it changes, an owner's
// first line has them all.
// refs/melee/src/sysdolphin/baselib/jobj.c::HSD_JObjMakeMatrix,
// HSD_JObjSetupMatrixSub

typedef struct TrackWorld {
    Mtx mtx;
    Vec3 scl;
    int has_scl;
} TrackWorld;

static int world_excluded(const HSD_JObj* j, int fighter)
{
    HSD_JObj* path = NULL;
    if (j->robj != NULL ||
        (j->flags & (JOBJ_JOINT | JOBJ_USER_DEF_MTX | JOBJ_MTX_INDEP_PARENT |
                     JOBJ_MTX_INDEP_SRT)))
    {
        return 1;
    }
    if (fighter) {
        if (j->flags & JOBJ_MSL_GAMEPLAY_COLD) {
            return 1;
        }
#ifdef MSL_POSE_ENGINE
        if (msl_fighter_pose_path(j, &path) && path != NULL) {
            return 1;
        }
        return 0;
#endif
    }
    if (j->aobj != NULL && j->aobj->hsd_obj != NULL) {
        return 1;
    }
    (void) path;
    return 0;
}

// The joint's matrix from its parent's (NULL at the root), as
// HSD_JObjMakeMatrix makes it.
static void world_make(const HSD_JObj* jc, const TrackWorld* parent,
                       TrackWorld* out)
{
    HSD_JObj* j = (HSD_JObj*) jc;
    Vec3* pscl = parent != NULL && parent->has_scl ? (Vec3*) &parent->scl
                                                   : NULL;
    if (j->flags & 8) {
        out->has_scl = pscl != NULL;
        if (pscl != NULL) {
            out->scl = *pscl;
        }
    } else {
        out->has_scl = 1;
        if (pscl != NULL) {
            out->scl.x = j->scale.x * pscl->x;
            out->scl.y = j->scale.y * pscl->y;
            out->scl.z = j->scale.z * pscl->z;
        } else {
            out->scl = j->scale;
        }
    }
    if (j->flags & JOBJ_USE_QUATERNION) {
        HSD_MtxSRTQuat(out->mtx, &j->scale, &j->rotate, &j->translate, pscl);
        if (parent != NULL) {
            PSMTXConcat((MtxPtr) parent->mtx, out->mtx, out->mtx);
        }
    } else if (parent != NULL) {
#ifdef MSL_CORE_NATIVE
        HSD_MtxSRTConcat(out->mtx, (MtxPtr) parent->mtx, &j->scale,
                         (Vec3*) &j->rotate, &j->translate, pscl);
#else
        HSD_MtxSRT(out->mtx, &j->scale, (Vec3*) &j->rotate, &j->translate,
                   pscl);
        PSMTXConcat((MtxPtr) parent->mtx, out->mtx, out->mtx);
#endif
    } else {
        HSD_MtxSRT(out->mtx, &j->scale, (Vec3*) &j->rotate, &j->translate,
                   NULL);
    }
}

// A joint's matrix made from the root down; 0 when it or one above it is
// left out.
static int world_of(const HSD_JObj* j, int fighter, TrackWorld* out)
{
    TrackWorld parent;
    if (j == NULL || world_excluded(j, fighter)) {
        return 0;
    }
    if (j->parent != NULL) {
        if (!world_of(j->parent, fighter, &parent)) {
            return 0;
        }
        world_make(j, &parent, out);
    } else {
        world_make(j, NULL, out);
    }
    return 1;
}

static void world_joint(MslViewerTrack* track, MslTrackPose* o, int at,
                        const TrackWorld* w)
{
    float v[12];
    int r;
    for (r = 0; r < 3; ++r) {
        v[r * 4 + 0] = w->mtx[r][0];
        v[r * 4 + 1] = w->mtx[r][1];
        v[r * 4 + 2] = w->mtx[r][2];
        v[r * 4 + 3] = w->mtx[r][3];
    }
    pose_joint(track, o, at, -1, v, 12);
}

static void world_fighter(MslViewerTrack* track, int frame, Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    int port = fp->player_id;
    int sub = fp->x221F_b4 ? 1 : 0;
    MslTrackPose* o;
    char head[64];
    int count;
    int part;
    if (fp->parts == NULL) {
        return;
    }
    o = owner_in(track->world, TRACK_WORLD_OWNERS, 'F', port * 2 + sub,
                 frame);
    if (o == NULL) {
        return;
    }
    if (o->character != (int) fp->kind + 1) {
        memset(o->known, 0, sizeof(o->known));
        o->character = (int) fp->kind + 1;
    }
    count = (int) ftPartsTable[fp->kind]->parts_num;
    if (count > TRACK_POSE_JOINTS) {
        count = TRACK_POSE_JOINTS;
    }
    for (part = 0; part < count; ++part) {
        TrackWorld w;
        if (world_of(fp->parts[part].joint, 1, &w)) {
            world_joint(track, o, part, &w);
        }
    }
    snprintf(head, sizeof(head), "[%d,\"wf\",%d,%d,%d", frame, port, sub,
             (int) fp->kind);
    pose_flush(track, head);
}

// An item's joints in its file's preorder, as pose_item_walk counts them,
// the root 0 too.
static void world_item_walk(MslViewerTrack* track, MslTrackPose* o,
                            HSD_JObj* j, HSD_Joint* d, int* index)
{
    for (; d != NULL; d = d->next, j = j != NULL ? j->next : NULL) {
        int at = (*index)++;
        TrackWorld w;
        if (at < TRACK_POSE_JOINTS && j != NULL && world_of(j, 0, &w)) {
            world_joint(track, o, at, &w);
        }
        if (d->child != NULL) {
            world_item_walk(track, o,
                            j != NULL && !(j->flags & JOBJ_INSTANCE)
                                ? j->child
                                : NULL,
                            d->child, index);
        }
    }
}

static void world_stage_walk(MslViewerTrack* track, MslTrackPose* o,
                             HSD_JObj* j, const HSD_Joint* d, int* index)
{
    for (; d != NULL && j != NULL; d = d->next, j = j->next) {
        int at = (*index)++;
        TrackWorld w;
        if (at < TRACK_POSE_JOINTS && world_of(j, 0, &w)) {
            world_joint(track, o, at, &w);
        }
        if (!(j->flags & JOBJ_INSTANCE)) {
            world_stage_walk(track, o, j->child, d->child, index);
        }
    }
}

static void track_world(MslViewerTrack* track, int frame,
                        const MslCoreMatch* match)
{
    Item_GObj* item;
    HSD_GObj* gobj;
    int others[64];
    char head[64];
    int i;
    for (i = 0; i < match->config.num_players; ++i) {
        if (match->fighters[i] != NULL) {
            world_fighter(track, frame, match->fighters[i]);
        }
        if (match->follower_fighters[i] != NULL &&
            !GET_FIGHTER(match->follower_fighters[i])->x221F_b3)
        {
            world_fighter(track, frame, match->follower_fighters[i]);
        }
    }
    for (item = (Item_GObj*) match->gobj.entities->items; item != NULL;
         item = (Item_GObj*) item->next)
    {
        Item* ip = GET_ITEM(item);
        HSD_JObj* root = GET_JOBJ(item);
        MslTrackPose* o;
        int index = 0;
        if (root == NULL || ip->xC8_joint == NULL) {
            continue;
        }
        o = owner_in(track->world, TRACK_WORLD_OWNERS, 'I', (int) ip->x1C,
                     frame);
        if (o == NULL) {
            continue;
        }
        world_item_walk(track, o, root, ip->xC8_joint, &index);
        snprintf(head, sizeof(head), "[%d,\"wi\",%d", frame, (int) ip->x1C);
        pose_flush(track, head);
    }
    memset(others, 0, sizeof(others));
    for (gobj = (HSD_GObj*) match->gobj.entities->x14; gobj != NULL;
         gobj = gobj->next)
    {
        UnkArchiveStruct* archive;
        const HSD_Joint* desc;
        HSD_JObj* root;
        MslTrackPose* o;
        int map_id;
        int id;
        int index = 1;
        TrackWorld w;
        if (gobj->classifier != HSD_GOBJ_CLASS_STAGE ||
            gobj->user_data == NULL || gobj->hsd_obj == NULL)
        {
            continue;
        }
        map_id = (int) GET_GROUND(gobj)->map_id;
        if (map_id < 0 || map_id >= 64) {
            continue;
        }
        id = map_id;
        if (stage_info.x180[map_id] != gobj) {
            id = map_id + 1000 * ++others[map_id];
        }
        archive = grDatFiles_801C6330(map_id);
        if (archive == NULL || archive->unk4 == NULL ||
            map_id >= archive->unk4->unkC)
        {
            continue;
        }
        desc = archive->unk4->unk8[map_id].unk0;
        root = gobj->hsd_obj;
        if (desc == NULL || stage_animation_stopped(gobj)) {
            continue;
        }
        o = owner_in(track->world, TRACK_WORLD_OWNERS, 'S', id, frame);
        if (o == NULL) {
            continue;
        }
        if (world_of(root, 0, &w)) {
            world_joint(track, o, 0, &w);
        }
        world_stage_walk(track, o, root->child, desc, &index);
        snprintf(head, sizeof(head), "[%d,\"ws\",%d", frame, id);
        pose_flush(track, head);
    }
    // (an owner not seen this frame is gone: its place is free)
    for (i = 0; i < TRACK_WORLD_OWNERS; ++i) {
        if (track->world[i].used && track->world[i].frame != frame) {
            track->world[i].used = 0;
        }
    }
}

static void track_poses(MslViewerTrack* track, int frame,
                        const MslCoreMatch* match)
{
    int i;
    for (i = 0; i < match->config.num_players; ++i) {
        if (match->fighters[i] != NULL) {
            track_fighter_joints(track, frame, match->fighters[i]);
            track_fighter_poses(track, frame, match->fighters[i]);
        }
        if (match->follower_fighters[i] != NULL &&
            !GET_FIGHTER(match->follower_fighters[i])->x221F_b3)
        {
            track_fighter_joints(track, frame, match->follower_fighters[i]);
            track_fighter_poses(track, frame, match->follower_fighters[i]);
        }
    }
    track_item_poses(track, frame, match);
    track_stage_poses(track, frame, match);
    if (track->world != NULL) {
        track_world(track, frame, match);
    }
    // (an owner not seen this frame is gone: its place is free)
    for (i = 0; i < TRACK_POSE_OWNERS; ++i) {
        if (track->pose[i].used && track->pose[i].frame != frame) {
            track->pose[i].used = 0;
        }
    }
}

// Struct dump (a debugging aid, off unless asked): with MSL_STRUCT_DUMP="f,f,.." (game frames, the track's numbering) and
// MSL_STRUCT_DUMP_FILE=path set in the environment as the track is made, one block of text lines is appended to the file
// for each of those frames, after the frame's gameplay:
//   fighter  every fighter and follower: kind, motion, anim_id, x594 (raw word), cur_pos, x68C/x698/x6A4, the hide bits,
//            x2226_b2, x221F_b0, the camera box
//   item     every item: kind, state, anim_id, spawn, position, velocity, holder, then
//   jobj     every JObj of its tree (depth, preorder index, flags, rotate, scale, translate, its RObjs, its AObj frame)
//   hud      the simulator's own state behind the HUD: per port the damage, stocks and the stock icons' animation;
//            per fighter the magnifier's offscreen reading (ifMagnify_802FC998) and its camera box
// Nothing here writes simulation state. Without the variables the function returns at its first test.
static void struct_dump_jobj(FILE* out, int frame, int owner, int depth, int* index, HSD_JObj* jo)
{
    for (; jo != NULL; jo = jo->next) {
        HSD_RObj* rr;
        fprintf(out,
                "jobj frame=%d owner=%d depth=%d idx=%d flags=%08x rotate=%.9g,%.9g,%.9g,%.9g scale=%.9g,%.9g,%.9g "
                "translate=%.9g,%.9g,%.9g",
                frame, owner, depth, (*index)++, (unsigned) jo->flags, jo->rotate.x, jo->rotate.y, jo->rotate.z, jo->rotate.w,
                jo->scale.x, jo->scale.y, jo->scale.z, jo->translate.x, jo->translate.y, jo->translate.z);
        for (rr = jo->robj; rr != NULL; rr = rr->next) {
            fprintf(out, " robj{flags=%08x}", (unsigned) rr->flags);
        }
        if (jo->aobj != NULL) {
            fprintf(out, " aobj{curr=%.9g end=%.9g rate=%.9g flags=%08x}", jo->aobj->curr_frame, jo->aobj->end_frame,
                    jo->aobj->framerate, (unsigned) jo->aobj->flags);
        } else {
            fprintf(out, " aobj=NULL");
        }
        fprintf(out, "\n");
        if (jo->child != NULL) {
            struct_dump_jobj(out, frame, owner, depth + 1, index, jo->child);
        }
    }
}

static void struct_dump(MslCoreMatch* match, int frame)
{
    static int armed = -1;
    static const char* list;
    static FILE* out;
    char key[32];
    char padded[4096];
    int i;

    if (armed < 0) {
        const char* path = getenv("MSL_STRUCT_DUMP_FILE");
        list = getenv("MSL_STRUCT_DUMP");
        armed = (list != NULL && path != NULL && (out = fopen(path, "a")) != NULL) ? 1 : 0;
    }
    if (!armed) {
        return;
    }
    snprintf(key, sizeof(key), ",%d,", frame);
    snprintf(padded, sizeof(padded), ",%s,", list);
    if (strstr(padded, key) == NULL) {
        return;
    }
    for (i = 0; i < match->config.num_players; ++i) {
        int sub;
        for (sub = 0; sub < 2; ++sub) {
            Fighter_GObj* g = sub ? match->follower_fighters[i] : match->fighters[i];
            Fighter* fp;
            CmSubject* cb;
            if (g == NULL) {
                continue;
            }
            fp = GET_FIGHTER(g);
            cb = fp->x890_cameraBox;
            fprintf(out,
                    "fighter frame=%d port=%d sub=%d kind=%d motion=%d anim_id=%d x594=%08x cur_pos=%.9g,%.9g,%.9g "
                    "x68C_transNPos=%.9g,%.9g,%.9g x698=%.9g,%.9g,%.9g x6A4_transNOffset=%.9g,%.9g,%.9g "
                    "invisible=%d x221E_b3=%d x221E_b4=%d x221E_b5=%d x221F_b0=%d x221F_b1=%d x221F_b3=%d x221F_b4=%d "
                    "x2220_b7=%d x2226_b2=%d x2226_b5=%d x2229_b3=%d "
                    "cb=%s cb.x8=%d cb.xC=%d%d%d cb.xE=%d cb.x10=%.9g,%.9g,%.9g cb.x1C=%.9g,%.9g,%.9g cb.x28=%.9g "
                    "cb.x2C=%.9g,%.9g cb.x34=%.9g,%.9g,%.9g\n",
                    frame, i, sub, (int) fp->kind, (int) fp->motion_id, (int) fp->anim_id, (unsigned) fp->x594_s32,
                    fp->cur_pos.x, fp->cur_pos.y, fp->cur_pos.z, fp->x68C_transNPos.x, fp->x68C_transNPos.y,
                    fp->x68C_transNPos.z, fp->x698.x, fp->x698.y, fp->x698.z, fp->x6A4_transNOffset.x,
                    fp->x6A4_transNOffset.y, fp->x6A4_transNOffset.z, (int) fp->invisible, (int) fp->x221E_b3,
                    (int) fp->x221E_b4, (int) fp->x221E_b5, (int) fp->x221F_b0, (int) fp->x221F_b1, (int) fp->x221F_b3,
                    (int) fp->x221F_b4, (int) fp->x2220_b7, (int) fp->x2226_b2, (int) fp->x2226_b5, (int) fp->x2229_b3,
                    cb != NULL ? "set" : "NULL", cb != NULL ? (int) cb->x8 : -1, cb != NULL ? (int) cb->xC_b0 : -1,
                    cb != NULL ? (int) cb->xC_b1 : -1, cb != NULL ? (int) cb->xC_b2 : -1, cb != NULL ? (int) cb->xE : -1,
                    cb != NULL ? cb->x10.x : 0.0F, cb != NULL ? cb->x10.y : 0.0F, cb != NULL ? cb->x10.z : 0.0F,
                    cb != NULL ? cb->x1C.x : 0.0F, cb != NULL ? cb->x1C.y : 0.0F, cb != NULL ? cb->x1C.z : 0.0F,
                    cb != NULL ? cb->x28 : 0.0F, cb != NULL ? cb->x2C.x : 0.0F, cb != NULL ? cb->x2C.y : 0.0F,
                    cb != NULL ? cb->x34.x : 0.0F, cb != NULL ? cb->x34.y : 0.0F, cb != NULL ? cb->x34.z : 0.0F);
        }
    }
    {
        Item_GObj* ig = (Item_GObj*) match->gobj.entities->items;
        for (; ig != NULL; ig = (Item_GObj*) ((HSD_GObj*) ig)->next) {
            Item* ip = GET_ITEM(ig);
            int index = 0;
            fprintf(out,
                    "item frame=%d kind=%d state=%d anim_id=%d spawn_id=%d pos=%.9g,%.9g,%.9g vel=%.9g,%.9g owner=%d\n",
                    frame, (int) ip->kind, (int) ip->msid, (int) ip->anim_id, (int) ip->x1C, ip->pos.x, ip->pos.y,
                    ip->pos.z, ip->x40_vel.x, ip->x40_vel.y, ip->owner != NULL ? (int) GET_FIGHTER(ip->owner)->player_id : -1);
            struct_dump_jobj(out, frame, (int) ip->x1C, 0, &index, GET_JOBJ(ig));
        }
    }
    for (i = 0; i < 4; ++i) {
        int k;
        fprintf(out, "hud frame=%d port=%d damage=%d stocks=%d icons_seen=%d anim=", frame, i, (int) Player_GetDamage(i),
                (int) Player_GetStocks(i), (int) track_hud[i].seen);
        for (k = 0; k < 5; ++k) {
            fprintf(out, "%s%d", k ? "," : "", (int) track_hud[i].anim[k]);
        }
        fprintf(out, " magnify_offscreen=%d\n", (int) ifMagnify_802FC998(i));
    }
    fflush(out);
}

void msl_viewer_track_frame(MslCoreMatch* match)
{
    int frame;
    int i;

    if (track_out == NULL) {
        return;
    }
    msl_core_bind_match(match);
    frame = match->frame_id;
    struct_dump(match, frame);
    for (i = 0; i < match->config.num_players; ++i) {
        if (match->fighters[i] != NULL) {
            track_fighter(frame, match->fighters[i]);
        }
        if (match->follower_fighters[i] != NULL &&
            !GET_FIGHTER(match->follower_fighters[i])->x221F_b3)
        {
            track_fighter(frame, match->follower_fighters[i]);
        }
    }
    track_stage(frame, match);
    track_items(frame, match);
    track_poses(&track_record, frame, match);
    if (!msl_viewer_track_plain()) {
        track_item_joints(frame, match);
    }
    // The random state after the frame's gameplay: what the particles'
    // update (efLib_particles_proc_main, the last to draw in a frame) starts
    // from. The replay gives each frame a fresh seed, so this is not the
    // next frame's.
    fprintf(track_out, "[%d,\"rng\",%u]\n", frame, (unsigned) seed_now());
#if MSL_GX_RECORD
    // The viewer build runs the particle update as the game does, a process of the efLib objects at link 0xF
    // (present/ps_disp.c::msl_present_ef_proc_main), from the random state of that point of the frame.
#else
    msl_particle_probe_frame(frame, seed_now(), track_skipped);
#endif
    track_skipped = 0;
    if (!msl_viewer_track_plain()) {
        track_hud_step(frame, match);
    }
#if !MSL_GX_RECORD
    msl_particle_probe_frame_written(frame);
#endif
}

int msl_viewer_track_resync(MslCoreMatch* match, char kind, int lane,
                            int field, float value)
{
    if (kind == 'i') {
        Item_GObj* gobj = (Item_GObj*) match->gobj.entities->items;
        int slot;
        for (slot = 0; gobj != NULL && slot < lane; ++slot) {
            gobj = (Item_GObj*) gobj->next;
        }
        if (gobj == NULL) {
            return -1;
        }
        switch (field) {
        case 0: GET_ITEM(gobj)->pos.x = value; break;
        case 1: GET_ITEM(gobj)->pos.y = value; break;
        case 2: GET_ITEM(gobj)->x40_vel.x = value; break;
        default: GET_ITEM(gobj)->x40_vel.y = value; break;
        }
        return 0;
    }
    if ((kind == 'f' || kind == 'n') && lane >= 0 &&
        lane < match->config.num_players)
    {
        HSD_GObj* gobj = kind == 'f' ? match->fighters[lane]
                                     : match->follower_fighters[lane];
        Fighter* fp;
        if (gobj == NULL) {
            return -1;
        }
        fp = GET_FIGHTER(gobj);
        if (field == 0) {
            fp->cur_pos.x = value;
        } else {
            fp->cur_pos.y = value;
        }
        return 0;
    }
    return -1;
}

#if MSL_GX_RECORD
// check (viewer build only, env MSL_W4_CHECK=1): after a frame's display pass, each fighter part's joint matrix
// (HSD_JObjSetupMatrix at draw) against the world matrix this file makes from the joint's values (track_world), bit for
// bit, plus the model's draw objects counted.
//   refs/melee/src/sysdolphin/baselib/jobj.c::HSD_JObjSetupMatrixSub
void msl_w4_check_fighters(const MslCoreMatch* match, int frame)
{
    static long dirty_count, checked, same, differ, excluded, parts_total, dobj_total, no_dobj_parts;
    static int first_report;
    int i;
    int sub;
    for (i = 0; i < match->config.num_players; ++i) {
        for (sub = 0; sub < 2; ++sub) {
            Fighter_GObj* gobj = sub ? match->follower_fighters[i] : match->fighters[i];
            Fighter* fp;
            int part;
            int count;
            if (gobj == NULL) {
                continue;
            }
            fp = GET_FIGHTER(gobj);
            if (fp->parts == NULL) {
                continue;
            }
            count = (int) ftPartsTable[fp->kind]->parts_num;
            for (part = 0; part < count; ++part) {
                HSD_JObj* j = fp->parts[part].joint;
                TrackWorld w;
                int r;
                int c;
                int bad = 0;
                parts_total++;
                if (j == NULL) {
                    continue;
                }
                if (j->flags & JOBJ_INSTANCE) {
                    continue;
                }
                if (!world_of(j, 1, &w)) {
                    excluded++;
                    continue;
                }
                if (HSD_JObjMtxIsDirty(j)) {
                    /* (a joint the draw did not need: its matrix is made now, as the game's next reader would) */
                    dirty_count++;
                    HSD_JObjSetupMatrix(j);
                }
                checked++;
                for (r = 0; r < 3; ++r) {
                    for (c = 0; c < 4; ++c) {
                        if (memcmp(&w.mtx[r][c], &j->mtx[r][c], 4) != 0) {
                            bad = 1;
                        }
                    }
                }
                if (bad) {
                    differ++;
                    if (first_report < 12) {
                        first_report++;
                        fprintf(stderr, "w4: frame %d fighter %d.%d part %d differs\n", frame, i, sub, part);
                        for (r = 0; r < 3; ++r) {
                            fprintf(stderr, "   mine %g %g %g %g | jobj %g %g %g %g\n", w.mtx[r][0], w.mtx[r][1],
                                    w.mtx[r][2], w.mtx[r][3], j->mtx[r][0], j->mtx[r][1], j->mtx[r][2],
                                    j->mtx[r][3]);
                        }
                    }
                } else {
                    same++;
                }
            }
        }
    }
    (void) dobj_total;
    (void) no_dobj_parts;
    if (frame % 200 == 0) {
        fprintf(stderr, "w4: frame %d parts %ld checked %ld same %ld differ %ld excluded %ld\n", frame, parts_total,
                checked, same, differ, excluded);
    }
}
#endif

#if MSL_GX_RECORD
// structure check (viewer build only, env MSL_W4_CHECK=1, at the first recorded frame): each fighter's main model as
// built (the filtered load with its draw objects) against a fresh HSD_JObjLoadJoint of the same costume joint, which is how
// the game builds it (Fighter_UnkUpdateCostumeJoint): joint tree, flags, draw objects, materials, textures, polygons.
//   refs/melee/src/melee/ft/fighter.c::Fighter_UnkUpdateCostumeJoint_800686E4
#include "ft/ftparts.h"
typedef struct W4Cmp {
    long joints, dobjs, mobjs, tobjs, pobjs, bad, metal_extra;
    int reported;
} W4Cmp;

static void w4_bad(W4Cmp* c, const char* what, long where)
{
    c->bad++;
    if (c->reported < 20) {
        c->reported++;
        fprintf(stderr, "w4: structure differs: %s (joint #%ld)\n", what, where);
    }
}

static void w4_cmp_dobj(W4Cmp* c, HSD_DObj* a, HSD_DObj* b, long where)
{
    for (; a != NULL && b != NULL; a = a->next, b = b->next) {
        static int dbg;
        HSD_MObj* ma = a->mobj;
        HSD_MObj* mb = b->mobj;
        HSD_PObj* pa = a->pobj;
        HSD_PObj* pb = b->pobj;
        c->dobjs++;
        if (where == 1 && getenv("MSL_W4_JOINTS") != NULL && c->dobjs > 60 && c->dobjs < 76) {
            fprintf(stderr, "w4:   dobj %ld live disp %p mobj %p | fresh disp %p mobj %p\n", c->dobjs, a->pobj ? (void*)a->pobj->display : 0, (void*)a->mobj, b->pobj ? (void*)b->pobj->display : 0, (void*)b->mobj);
            dbg++;
        }
        if ((a->flags & ~1u) != (b->flags & ~1u)) {
            w4_bad(c, "dobj flags", where);
        }
        if ((ma == NULL) != (mb == NULL)) {
            w4_bad(c, "mobj presence", where);
        } else if (ma != NULL) {
            HSD_TObj* ta = ma->tobj;
            HSD_TObj* tb = mb->tobj;
            c->mobjs++;
            if (ma->rendermode != mb->rendermode ||
                memcmp(ma->mat, mb->mat, sizeof(HSD_Material)) != 0)
            {
                w4_bad(c, "mobj rendermode/material", where);
            }
            if ((ma->pe == NULL) != (mb->pe == NULL) ||
                (ma->pe != NULL && memcmp(ma->pe, mb->pe, sizeof(HSD_PEDesc)) != 0))
            {
                w4_bad(c, "mobj pe", where);
            }
            for (; ta != NULL && tb != NULL; ta = ta->next, tb = tb->next) {
                c->tobjs++;
                if (ta->imagedesc != tb->imagedesc || (ta->tlut == NULL) != (tb->tlut == NULL) ||
                    (ta->tlut != NULL && (ta->tlut->lut != tb->tlut->lut || ta->tlut->fmt != tb->tlut->fmt || ta->tlut->n_entries != tb->tlut->n_entries)) || ta->flags != tb->flags ||
                    ta->wrap_s != tb->wrap_s || ta->wrap_t != tb->wrap_t || ta->id != tb->id ||
                    ta->src != tb->src || ta->lod != tb->lod ||
                    memcmp(&ta->translate, &tb->translate, 12) != 0 || memcmp(&ta->scale, &tb->scale, 12) != 0)
                {
                    fprintf(stderr, "w4: tobj live img %p tlut %p fl %x wrap %d %d id %d src %d lod %p | fresh img %p tlut %p fl %x wrap %d %d id %d src %d lod %p\n", (void*)ta->imagedesc,(void*)ta->tlut,ta->flags,ta->wrap_s,ta->wrap_t,ta->id,ta->src,(void*)ta->lod,(void*)tb->imagedesc,(void*)tb->tlut,tb->flags,tb->wrap_s,tb->wrap_t,tb->id,tb->src,(void*)tb->lod);
                    w4_bad(c, "tobj", where);
                }
            }
            if (ta != NULL || tb != NULL) {
                w4_bad(c, "tobj count", where);
            }
        }
        for (; pa != NULL && pb != NULL; pa = pa->next, pb = pb->next) {
            c->pobjs++;
            if (pa->flags != pb->flags || pa->n_display != pb->n_display || pa->display != pb->display ||
                pa->verts != pb->verts)
            {
                w4_bad(c, "pobj", where);
            }
        }
        if (pa != NULL || pb != NULL) {
            w4_bad(c, "pobj count", where);
        }
    }
    if (a != NULL || b != NULL) {
        int na = 0, nb = 0;
        /* (the metal fighter's draw objects, which ftMetal appends to the model: the game's own) */
        for (; a != NULL; a = a->next) na++;
        for (; b != NULL; b = b->next) nb++;
        c->metal_extra += na;
        if (nb != 0) {
            w4_bad(c, "dobj count", where);
        }
    }
}

static void w4_cmp_tree(W4Cmp* c, HSD_JObj* a, HSD_JObj* b)
{
    for (; a != NULL && b != NULL; a = a->next, b = b->next) {
        const u32 mask = ~(u32) (JOBJ_MTX_DIRTY | JOBJ_MSL_GAMEPLAY_COLD);
        c->joints++;
        if (c->joints <= 70 && getenv("MSL_W4_JOINTS") != NULL) {
            int na = 0, nb = 0;
            HSD_DObj* d;
            if (union_type_dobj(a)) for (d = a->u.dobj; d != NULL; d = d->next) na++;
            if (union_type_dobj(b)) for (d = b->u.dobj; d != NULL; d = d->next) nb++;
            fprintf(stderr, "w4: joint %ld live flags %08x dobjs %d | fresh flags %08x dobjs %d\n", c->joints, (unsigned) a->flags, na, (unsigned) b->flags, nb);
        }
        if ((a->flags & mask) != (b->flags & mask)) {
            w4_bad(c, "joint flags", c->joints);
        }
        if (union_type_dobj(a) && union_type_dobj(b)) {
            w4_cmp_dobj(c, a->u.dobj, b->u.dobj, c->joints);
        } else if (union_type_dobj(a) != union_type_dobj(b)) {
            w4_bad(c, "union type", c->joints);
        }
        if (!(a->flags & JOBJ_INSTANCE) && !(b->flags & JOBJ_INSTANCE)) {
            w4_cmp_tree(c, a->child, b->child);
        }
    }
    if (a != NULL || b != NULL) {
        w4_bad(c, "sibling count", c->joints);
    }
}

void msl_w4_check_structure(const MslCoreMatch* match)
{
    int i;
    int sub;
    for (i = 0; i < match->config.num_players; ++i) {
        for (sub = 0; sub < 2; ++sub) {
            Fighter_GObj* gobj = sub ? match->follower_fighters[i] : match->fighters[i];
            Fighter* fp;
            HSD_JObj* fresh;
            W4Cmp c;
            if (gobj == NULL) {
                continue;
            }
            fp = GET_FIGHTER(gobj);
            memset(&c, 0, sizeof(c));
            ftPartsPObjSetDefaultClass();
            fresh = HSD_JObjLoadJoint(fp->x108_costume_joint);
            w4_cmp_tree(&c, GET_JOBJ(gobj), fresh);
            fprintf(stderr, "w4: fighter %d.%d kind %d: joints %ld dobjs %ld mobjs %ld tobjs %ld pobjs %ld, differences %ld\n", i,
                    sub, (int) fp->kind, c.joints, c.dobjs, c.mobjs, c.tobjs, c.pobjs, c.bad);
            HSD_JObjRemoveAll(fresh);
        }
    }
}
#endif

#if MSL_GX_RECORD
// item check (env MSL_W4_CHECK=1): each item the frame first shows (items, fighter articles and projectiles are all
// items): its kind and the size of its JObj tree with the draw objects.
static int w4_item_tree(HSD_JObj* j, long* joints, long* dobjs, long* pobjs, long* tobjs)
{
    for (; j != NULL; j = j->next) {
        (*joints)++;
        if (union_type_dobj(j)) {
            HSD_DObj* d;
            for (d = j->u.dobj; d != NULL; d = d->next) {
                HSD_PObj* p;
                HSD_TObj* t;
                (*dobjs)++;
                for (p = d->pobj; p != NULL; p = p->next) (*pobjs)++;
                if (d->mobj != NULL) for (t = d->mobj->tobj; t != NULL; t = t->next) (*tobjs)++;
            }
        }
        if (!(j->flags & JOBJ_INSTANCE)) {
            w4_item_tree(j->child, joints, dobjs, pobjs, tobjs);
        }
    }
    return 0;
}

void msl_w4_check_items(const MslCoreMatch* match, int frame)
{
    static void* seen[512];
    static int seen_count;
    Item_GObj* item;
    for (item = (Item_GObj*) match->gobj.entities->items; item != NULL; item = (Item_GObj*) item->next) {
        Item* ip = GET_ITEM(item);
        HSD_JObj* root = GET_JOBJ(item);
        int live_a = 0, live_b = 0; const char* live = getenv("MSL_W4_LIVE");
        if (live != NULL && sscanf(live, "%d:%d", &live_a, &live_b) == 2 && frame >= live_a && frame < live_b) {
            fprintf(stderr, "w4: live frame %d item kind %d pos %g %g\n", frame, (int) ip->kind, ip->pos.x, ip->pos.y);
        }
        long joints = 0, dobjs = 0, pobjs = 0, tobjs = 0;
        int k;
        int found = 0;
        for (k = 0; k < seen_count; ++k) {
            if (seen[k] == (void*) item) found = 1;
        }
        if (found) continue;
        if (seen_count < 512) seen[seen_count++] = (void*) item;
        w4_item_tree(root, &joints, &dobjs, &pobjs, &tobjs);
        fprintf(stderr, "w4: frame %d new item kind %d spawn %d joint desc %s: joints %ld dobjs %ld pobjs %ld tobjs %ld\n", frame,
                (int) ip->kind, (int) ip->spawn_kind, ip->xC8_joint != NULL ? "yes" : "no", joints, dobjs, pobjs, tobjs);
    }
}
#endif

#if MSL_GX_RECORD
// position-matrix log (env MSL_W4_POSLOG=<file>): one line per position matrix a pobj loads in the draw pass:
//   kind (1 rigid, 2 envelope of one joint, 3 blended envelope), the fighter/part of the joint(s), the 12 words.
// The game's stream holds the same matrices in the same order; tools/posmat.py lines them up.
// refs/melee/src/sysdolphin/baselib/pobj.c::{SetupRigidModelMtx,SetupEnvelopeModelMtx}
static const MslCoreMatch* w4_log_match;
static int w4_log_frame;

void msl_w4_poslog_frame(const MslCoreMatch* match, int frame)
{
    w4_log_match = match;
    w4_log_frame = frame;
}

static void w4_where(const HSD_JObj* j, char* out, size_t room)
{
    int i;
    int sub;
    snprintf(out, room, "-");
    if (j == NULL || w4_log_match == NULL) {
        return;
    }
    for (i = 0; i < w4_log_match->config.num_players; ++i) {
        for (sub = 0; sub < 2; ++sub) {
            Fighter_GObj* g = sub ? w4_log_match->follower_fighters[i] : w4_log_match->fighters[i];
            int part;
            if (g == NULL || GET_FIGHTER(g)->parts == NULL) {
                continue;
            }
            for (part = 0; part < (int) ftPartsTable[GET_FIGHTER(g)->kind]->parts_num; ++part) {
                if (GET_FIGHTER(g)->parts[part].joint == j) {
                    snprintf(out, room, "f%d.%d:%d", i, sub, part);
                    return;
                }
            }
        }
    }
}

void msl_w4_poslog(int kind, HSD_JObj* a, HSD_JObj* b, Mtx m)
{
    static FILE* f;
    static int tried;
    char wa[32];
    char wb[32];
    int r;
    int c;
    if (!tried) {
        const char* path = getenv("MSL_W4_POSLOG");
        tried = 1;
        if (path != NULL) {
            f = fopen(path, "w");
        }
    }
    if (f == NULL) {
        return;
    }
    w4_where(a, wa, sizeof(wa));
    w4_where(b, wb, sizeof(wb));
    fprintf(f, "%d %d %s %s", w4_log_frame, kind, wa, wb);
    for (r = 0; r < 3; ++r) {
        for (c = 0; c < 4; ++c) {
            uint32_t w;
            memcpy(&w, &m[r][c], 4);
            fprintf(f, " %08x", w);
        }
    }
    if (a != NULL) {
        const uint32_t* q = (const uint32_t*) &a->rotate;
        const uint32_t* sc = (const uint32_t*) &a->scale;
        const uint32_t* tr = (const uint32_t*) &a->translate;
        fprintf(f, " | %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned) a->flags, q[0], q[1], q[2], sc[0], sc[1], sc[2], tr[0], tr[1], tr[2], a->parent != NULL ? (unsigned) a->parent->flags : 0u);
    }
    fputc(10, f);
}
#endif

#if MSL_GX_RECORD
// animation check (env MSL_W4_ANIM=a:b, frames a up to b): the material and texture animations on each fighter's draw objects: how many
// have an AObj and how far they have run, to see whether the generic per-frame DObj animation reaches them.
//   refs/melee/src/sysdolphin/baselib/jobj.c::HSD_JObjAnim (HSD_DObjAnimAll)
void msl_w4_check_anims(const MslCoreMatch* match, int frame)
{
    const char* range = getenv("MSL_W4_ANIM");
    int a = 0;
    int b = 0;
    int i;
    if (range == NULL || sscanf(range, "%d:%d", &a, &b) != 2 || frame < a || frame >= b) {
        return;
    }
    for (i = 0; i < match->config.num_players; ++i) {
        Fighter_GObj* g = match->fighters[i];
        Fighter* fp;
        u32 d;
        int mo = 0;
        int to = 0;
        double sum_m = 0.0;
        double sum_t = 0.0;
        if (g == NULL) {
            continue;
        }
        fp = GET_FIGHTER(g);
        for (d = 0; d < fp->dobj_list.count && d < 128; ++d) {
            HSD_DObj* dobj = fp->dobj_list.data[d];
            HSD_TObj* t;
            if (dobj == NULL || dobj->mobj == NULL) {
                continue;
            }
            if (dobj->mobj->aobj != NULL) {
                mo++;
                sum_m += dobj->mobj->aobj->curr_frame;
            }
            for (t = dobj->mobj->tobj; t != NULL; t = t->next) {
                if (t->aobj != NULL) {
                    to++;
                    sum_t += t->aobj->curr_frame;
                }
            }
        }
        fprintf(stderr, "w4: anim frame %d fighter %d kind %d: dobjs %u with mobj aobj %d (sum frame %g), tobj aobj %d (sum frame %g)\n", frame, i,
                (int) fp->kind, fp->dobj_list.count, mo, sum_m, to, sum_t);
    }
}
#endif

#if MSL_GX_RECORD
// hide switch (env MSL_W4_HIDE="port:index"): before each display pass that fighter's draw object is hidden (the DObj hidden bit),
// to find which object a primitive of the stream comes from. A second field, MSL_W4_HIDE="port:-1", hides all but none (a bring-up probe).
void msl_w4_hide(const MslCoreMatch* match)
{
    const char* spec = getenv("MSL_W4_HIDE");
    int port;
    int index;
    if (spec == NULL || sscanf(spec, "%d:%d", &port, &index) != 2) {
        return;
    }
    if (port >= 0 && port < match->config.num_players && match->fighters[port] != NULL) {
        Fighter* fp = GET_FIGHTER(match->fighters[port]);
        if (index >= 0 && (u32) index < fp->dobj_list.count) {
            HSD_DObj* dobj = fp->dobj_list.data[index];
            if (dobj != NULL) {
                dobj->flags |= 1;
            }
        }
    }
}

// How many draw objects a fighter has, for the probe above.
int msl_w4_dobj_count(const MslCoreMatch* match, int port)
{
    if (port >= 0 && port < match->config.num_players && match->fighters[port] != NULL) {
        return (int) GET_FIGHTER(match->fighters[port])->dobj_list.count;
    }
    return 0;
}
#endif

#if MSL_GX_RECORD
// chain dump (env MSL_W4_DYN=<tape frame>): every link of every fighter's dynamic bone sets at that frame's end (after the display pass):
//   fighter set link  rotate xyz (bits)  world position unk_2C xyz  angular velocity unk_44
// The game's memory snapshot (.dff.mem1) holds the same fields (tools/memdyn.py).
//   refs/melee/src/melee/lb/lbspdisplay.c::lb_8001044C
void msl_w4_dump_dynamics(const MslCoreMatch* match, int frame)
{
    const char* env = getenv("MSL_W4_DYN");
    int i;
    if (env == NULL || atoi(env) != frame) {
        return;
    }
    for (i = 0; i < match->config.num_players; ++i) {
        Fighter_GObj* g = match->fighters[i];
        Fighter* fp;
        ssize_t set;
        if (g == NULL) {
            continue;
        }
        fp = GET_FIGHTER(g);
        for (set = 0; set < fp->dynamics_num; ++set) {
            struct DynamicsData* d = fp->dynamic_bone_sets[set].dyn_desc.data;
            int link = 0;
            for (; d != NULL; d = d->next, ++link) {
                HSD_JObj* j = d->desc.lb_unk0.jobj;
                uint32_t w[7];
                if (j != NULL) {
                    memcpy(&w[0], &j->rotate.x, 4);
                    memcpy(&w[1], &j->rotate.y, 4);
                    memcpy(&w[2], &j->rotate.z, 4);
                } else {
                    w[0] = w[1] = w[2] = 0;
                }
                memcpy(&w[3], &d->desc.lb_unk0.unk_2C.x, 4);
                memcpy(&w[4], &d->desc.lb_unk0.unk_2C.y, 4);
                memcpy(&w[5], &d->desc.lb_unk0.unk_2C.z, 4);
                memcpy(&w[6], &d->desc.lb_unk0.unk_44, 4);
                fprintf(stderr, "dyn %d %d %d %08x %08x %08x %08x %08x %08x %08x\n", i, (int) set, link, w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
            }
        }
    }
}
#endif

#if MSL_GX_RECORD
// item joint dump (env MSL_W4_ITEMJ=<tape frame>): the joints of every item at that frame's end.
void msl_w4_dump_item_joints(const MslCoreMatch* match, int frame)
{
    const char* env = getenv("MSL_W4_ITEMJ");
    Item_GObj* item;
    if (env == NULL || atoi(env) != frame) {
        return;
    }
    for (item = (Item_GObj*) match->gobj.entities->items; item != NULL; item = (Item_GObj*) item->next) {
        Item* ip = GET_ITEM(item);
        HSD_JObj* stack[64];
        int sp = 0;
        int n = 0;
        stack[sp++] = GET_JOBJ(item);
        fprintf(stderr, "itemj item kind %d\n", (int) ip->kind);
        while (sp > 0) {
            HSD_JObj* j = stack[--sp];
            for (; j != NULL; j = j->next) {
                fprintf(stderr, "itemj %d flags %08x rot %.9g %.9g %.9g trans %.9g %.9g %.9g scale %.9g %.9g %.9g aobj %d\n", n++, (unsigned) j->flags,
                        j->rotate.x, j->rotate.y, j->rotate.z, j->translate.x, j->translate.y, j->translate.z, j->scale.x, j->scale.y,
                        j->scale.z, j->aobj != NULL);
                if (j->child != NULL && !(j->flags & JOBJ_INSTANCE) && sp < 63) {
                    stack[sp++] = j->child;
                }
            }
        }
    }
}
#endif

#if MSL_GX_RECORD
// state injection (env MSL_W4_INJECT=<file>, at the end of the tape frame named by MSL_W4_INJECT_FRAME): replaces the state of the dynamic bone links
// (the joint's euler rotation, the world position unk_2C, the rotation axis unk_38, the angular velocity unk_44) with the game's, from its memory snapshot,
// to see what one solver step does to an exact state. Lines: "<fighter> <set> <link> rx ry rz px py pz ax ay az av" as float bit patterns.
//   refs/melee/src/melee/lb/lbspdisplay.c::lb_8001044C
void msl_w4_inject_dynamics(const MslCoreMatch* match, int frame)
{
    const char* path = getenv("MSL_W4_INJECT");
    const char* at = getenv("MSL_W4_INJECT_FRAME");
    FILE* f;
    char line[512];
    if (path == NULL || at == NULL || atoi(at) != frame) {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned fighter;
        unsigned set;
        unsigned link;
        unsigned w[10];
        if (sscanf(line, "%u %u %u %x %x %x %x %x %x %x %x %x %x", &fighter, &set, &link, &w[0], &w[1], &w[2], &w[3], &w[4], &w[5], &w[6], &w[7],
                   &w[8], &w[9]) == 13 &&
            (int) fighter < match->config.num_players && match->fighters[fighter] != NULL)
        {
            Fighter* fp = GET_FIGHTER(match->fighters[fighter]);
            if ((ssize_t) set < fp->dynamics_num) {
                struct DynamicsData* d = fp->dynamic_bone_sets[set].dyn_desc.data;
                unsigned k;
                for (k = 0; k < link && d != NULL; ++k) {
                    d = d->next;
                }
                if (d != NULL && d->desc.lb_unk0.jobj != NULL) {
                    HSD_JObj* j = d->desc.lb_unk0.jobj;
                    memcpy(&j->rotate.x, &w[0], 4);
                    memcpy(&j->rotate.y, &w[1], 4);
                    memcpy(&j->rotate.z, &w[2], 4);
                    memcpy(&d->desc.lb_unk0.unk_2C.x, &w[3], 4);
                    memcpy(&d->desc.lb_unk0.unk_2C.y, &w[4], 4);
                    memcpy(&d->desc.lb_unk0.unk_2C.z, &w[5], 4);
                    memcpy(&d->desc.lb_unk0.unk_38.x, &w[6], 4);
                    memcpy(&d->desc.lb_unk0.unk_38.y, &w[7], 4);
                    memcpy(&d->desc.lb_unk0.unk_38.z, &w[8], 4);
                    memcpy(&d->desc.lb_unk0.unk_44, &w[9], 4);
                    HSD_JObjSetMtxDirty(j);
                }
            }
        }
    }
    fclose(f);
}
#endif

#if MSL_GX_RECORD
// full link dump (env MSL_W4_DYNALL=<tape frame>): every float field of each dynamic bone link, in the console's offset order:
//   dynall <fighter> <set> <link> then 0x04.. unk_58 etc.: rotate(4) translate(3) scale(3) unk_2C(3) unk_38(3) 44 48 4C 50 54 58(4) 68 6C(3) 78(3) 84 88 8C
void msl_w4_dump_dynamics_all(const MslCoreMatch* match, int frame)
{
    const char* env = getenv("MSL_W4_DYNALL");
    int i;
    if (env == NULL || atoi(env) != frame) {
        return;
    }
    for (i = 0; i < match->config.num_players; ++i) {
        Fighter_GObj* g = match->fighters[i];
        ssize_t set;
        if (g == NULL) {
            continue;
        }
        for (set = 0; set < GET_FIGHTER(g)->dynamics_num; ++set) {
            struct DynamicsData* d = GET_FIGHTER(g)->dynamic_bone_sets[set].dyn_desc.data;
            int link = 0;
            for (; d != NULL; d = d->next, ++link) {
                struct lb_00F9_UnkDesc0* u = &d->desc.lb_unk0;
                float v[44];
                int n = 0;
                int k;
                v[n++] = u->rotate.x; v[n++] = u->rotate.y; v[n++] = u->rotate.z; v[n++] = u->rotate.w;
                v[n++] = u->translate.x; v[n++] = u->translate.y; v[n++] = u->translate.z;
                v[n++] = u->scale.x; v[n++] = u->scale.y; v[n++] = u->scale.z;
                v[n++] = u->unk_2C.x; v[n++] = u->unk_2C.y; v[n++] = u->unk_2C.z;
                v[n++] = u->unk_38.x; v[n++] = u->unk_38.y; v[n++] = u->unk_38.z;
                v[n++] = u->unk_44; v[n++] = u->unk_48; v[n++] = u->unk_4C; v[n++] = u->unk_50;
                memcpy(&v[n++], &u->unk_54, 4);
                v[n++] = u->unk_58.x; v[n++] = u->unk_58.y; v[n++] = u->unk_58.z; v[n++] = u->unk_58.w;
                v[n++] = u->unk_68;
                v[n++] = u->unk_6C.x; v[n++] = u->unk_6C.y; v[n++] = u->unk_6C.z;
                v[n++] = u->unk_78.x; v[n++] = u->unk_78.y; v[n++] = u->unk_78.z;
                v[n++] = u->unk_84; v[n++] = u->unk_88; v[n++] = u->unk_8C;
                fprintf(stderr, "dynall %d %d %d", i, (int) set, link);
                for (k = 0; k < n; ++k) {
                    uint32_t w;
                    memcpy(&w, &v[k], 4);
                    fprintf(stderr, " %08x", w);
                }
                {
                    HSD_JObj* jo = u->jobj;
                    const float* jf[9];
                    uint32_t fl;
                    jf[0] = &jo->rotate.x; jf[1] = &jo->rotate.y; jf[2] = &jo->rotate.z;
                    jf[3] = &jo->scale.x; jf[4] = &jo->scale.y; jf[5] = &jo->scale.z;
                    jf[6] = &jo->translate.x; jf[7] = &jo->translate.y; jf[8] = &jo->translate.z;
                    for (k = 0; k < 9; ++k) {
                        uint32_t w;
                        memcpy(&w, jf[k], 4);
                        fprintf(stderr, " %08x", w);
                    }
                    fl = jo->flags;
                    fprintf(stderr, " %08x", fl);
                    {
                        int pi;
                        int found = -1;
                        for (pi = 0; pi < (int) ftPartsTable[GET_FIGHTER(g)->kind]->parts_num; ++pi) {
                            if (GET_FIGHTER(g)->parts[pi].joint == jo) {
                                found = pi;
                            }
                        }
                        fprintf(stderr, " part%d", found);
                    }
                }
                fputc(10, stderr);
            }
        }
    }
}
#endif

#if MSL_GX_RECORD
// rotation bump (env MSL_W4_BUMP="fighter set link kx ky kz" at tape frame MSL_W4_BUMP_FRAME, before that frame's display pass): moves a dynamic bone
// joint's euler rotation by whole ulps, to find how far a solver step is from the console's.
void msl_w4_bump(const MslCoreMatch* match, int frame)
{
    const char* spec = getenv("MSL_W4_BUMP");
    const char* at = getenv("MSL_W4_BUMP_FRAME");
    unsigned fighter;
    unsigned set;
    unsigned link;
    int k[3];
    if (spec == NULL || at == NULL || atoi(at) != frame ||
        sscanf(spec, "%u %u %u %d %d %d", &fighter, &set, &link, &k[0], &k[1], &k[2]) != 6 ||
        (int) fighter >= match->config.num_players || match->fighters[fighter] == NULL)
    {
        return;
    }
    {
        Fighter* fp = GET_FIGHTER(match->fighters[fighter]);
        struct DynamicsData* d;
        unsigned n;
        if ((ssize_t) set >= fp->dynamics_num) {
            return;
        }
        d = fp->dynamic_bone_sets[set].dyn_desc.data;
        for (n = 0; n < link && d != NULL; ++n) {
            d = d->next;
        }
        if (d != NULL && d->desc.lb_unk0.jobj != NULL) {
            float* r = &d->desc.lb_unk0.jobj->rotate.x;
            int c;
            for (c = 0; c < 3; ++c) {
                uint32_t b;
                int32_t key;
                memcpy(&b, &r[c], 4);
                key = (b & 0x80000000u) ? -(int32_t) (b & 0x7FFFFFFFu) : (int32_t) b;
                key += k[c];
                b = key < 0 ? ((uint32_t) -key | 0x80000000u) : (uint32_t) key;
                memcpy(&r[c], &b, 4);
            }
            HSD_JObjSetMtxDirty(d->desc.lb_unk0.jobj);
        }
    }
}
#endif

#if MSL_GX_RECORD
// fighter joint dump (env MSL_W4_JMTX=<tape frame>): each fighter's joint tree in depth-first order (child, then next), one line per joint:
//   jmtx <fighter> <index> <depth> <flags> rot(3) scale(3) trans(3) mtx(12), float bit patterns (tools/jointdiff.py lines this up with the game's memory snapshot).
static void w4_dump_joint_tree(int fighter, HSD_JObj* j, int depth, int* n)
{
    for (; j != NULL; j = j->next) {
        const uint32_t* w[4];
        int k;
        w[0] = (const uint32_t*) &j->rotate;
        w[1] = (const uint32_t*) &j->scale;
        w[2] = (const uint32_t*) &j->translate;
        w[3] = (const uint32_t*) &j->mtx[0][0];
        fprintf(stderr, "jmtx %d %d %d %08x", fighter, (*n)++, depth, (unsigned) j->flags);
        for (k = 0; k < 3; ++k) fprintf(stderr, " %08x", w[0][k]);
        for (k = 0; k < 3; ++k) fprintf(stderr, " %08x", w[1][k]);
        for (k = 0; k < 3; ++k) fprintf(stderr, " %08x", w[2][k]);
        for (k = 0; k < 12; ++k) fprintf(stderr, " %08x", w[3][k]);
        fprintf(stderr, "\n");
        if (j->child != NULL && !(j->flags & JOBJ_INSTANCE)) {
            w4_dump_joint_tree(fighter, j->child, depth + 1, n);
        }
    }
}

void msl_w4_dump_joints(const MslCoreMatch* match, int frame)
{
    const char* env = getenv("MSL_W4_JMTX");
    int i;
    if (env == NULL || atoi(env) != frame) {
        return;
    }
    for (i = 0; i < match->config.num_players; ++i) {
        Fighter_GObj* g = match->fighters[i];
        int n = 0;
        if (g != NULL) {
            HSD_JObj* root = GET_JOBJ(g);
            w4_dump_joint_tree(i, root, 0, &n);
        }
    }
}
#endif

#if MSL_GX_RECORD
// world joint dump (env MSL_W4_ALLJ=<tape frame>): every joint of every drawn object (the GX links' objects), depth first:
//   allj <gx link> <class> <joint index> <depth> <flags> rot(3) scale(3) trans(3) mtx(12) as float bit patterns (tools/alljdiff.py against the game's snapshot).
static void w4_dump_all_tree(int link, int cls, HSD_JObj* j, int depth, int* n)
{
    for (; j != NULL; j = j->next) {
        const uint32_t* w[4];
        int k;
        w[0] = (const uint32_t*) &j->rotate;
        w[1] = (const uint32_t*) &j->scale;
        w[2] = (const uint32_t*) &j->translate;
        w[3] = (const uint32_t*) &j->mtx[0][0];
        fprintf(stderr, "allj %d %d %d %d %08x", link, cls, (*n)++, depth, (unsigned) j->flags);
        for (k = 0; k < 3; ++k) fprintf(stderr, " %08x", w[0][k]);
        for (k = 0; k < 3; ++k) fprintf(stderr, " %08x", w[1][k]);
        for (k = 0; k < 3; ++k) fprintf(stderr, " %08x", w[2][k]);
        for (k = 0; k < 12; ++k) fprintf(stderr, " %08x", w[3][k]);
        fprintf(stderr, "\n");
        if (j->child != NULL && !(j->flags & JOBJ_INSTANCE)) {
            w4_dump_all_tree(link, cls, j->child, depth + 1, n);
        }
        if (depth == 0) {
            break;
        }
    }
}

void msl_w4_dump_all_joints(const MslCoreMatch* match, int frame)
{
    const char* env = getenv("MSL_W4_ALLJ");
    int link;
    (void) match;
    if (env == NULL || atoi(env) != frame) {
        return;
    }
    for (link = 0; link <= HSD_GObjLibInitData.gx_link_max + 1; ++link) {
        HSD_GObj* g;
        for (g = HSD_GObjGXLinkHead[link]; g != NULL; g = g->next_gx) {
            int n = 0;
            if (g->hsd_obj != NULL) {
                w4_dump_all_tree(link, (int) g->classifier, (HSD_JObj*) g->hsd_obj, 0, &n);
            }
        }
    }
}
#endif

#if MSL_GX_RECORD
// stale-matrix probe (env MSL_W4_STALE=<tape step>): at the named point of the frame, the first joints of fighter 0 whose flags say clean while their world matrix is not parent * local.
extern void HSD_MtxSRT(Mtx m, Vec3* scale, Vec3* rot, Vec3* trans, Vec3* parent_scale);
extern int msl_w4_cur_step;
static void w4_stale_walk(HSD_JObj* j, int depth, int* n, int* shown, const char* tag)
{
    for (; j != NULL; j = j->next) {
        if (j->parent != NULL && !(j->flags & JOBJ_MTX_DIRTY) && !(j->flags & (JOBJ_USER_DEF_MTX | JOBJ_MTX_INDEP_PARENT | 0x20000)) && j->robj == NULL) {
            Mtx loc;
            Mtx exp;
            int r;
            int c;
            int stale = 0;
            unsigned mask = 0;
            HSD_MtxSRT(loc, &j->scale, (Vec3*) &j->rotate, &j->translate, NULL);
            PSMTXConcat(j->parent->mtx, loc, exp);
            for (r = 0; r < 3; ++r) for (c = 0; c < 4; ++c) if (memcmp(&exp[r][c], &j->mtx[r][c], 4) != 0) { stale = 1; mask |= 1u << (r * 4 + c); }
            if (stale && *shown < 6) {
                fprintf(stderr, "stale %s joint %d depth %d mask %03x flags %08x parent flags %08x\n", tag, *n, depth, mask, (unsigned) j->flags, (unsigned) j->parent->flags);
                ++*shown;
            }
        }
        ++*n;
        if (j->child != NULL && !(j->flags & JOBJ_INSTANCE)) {
            w4_stale_walk(j->child, depth + 1, n, shown, tag);
        }
        if (depth == 0) break;
    }
}

void msl_w4_stale_probe(Fighter_GObj* g, const char* tag)
{
    static int want = -2;
    int n = 0;
    int shown = 0;
    if (want == -2) {
        const char* v = getenv("MSL_W4_STALE");
        want = v != NULL ? atoi(v) : -1;
    }
    if (want != msl_w4_cur_step) {
        return;
    }
    {
        char full[96];
        Fighter_GObj* f0 = (Fighter_GObj*) Player_GetEntityAtIndex(0, 0);
        snprintf(full, sizeof(full), "%s(by p%d)", tag, (int) GET_FIGHTER(g)->player_id);
        w4_stale_walk(GET_JOBJ(f0), 0, &n, &shown, full);
        tag = full;
    }
    if (shown == 0) {
        fprintf(stderr, "stale %s none\n", tag);
    }
}
#endif

#if MSL_GX_RECORD
__attribute__((noinline)) void msl_w4_gdb_trap(HSD_JObj* j)
{
    __asm__ volatile("" ::: "memory");
    (void) j;
}

// probe (env MSL_W4_MK=<translate.x bits in hex>): every matrix make of a joint with that translate.x: step, rotate, parent matrix, result.
void msl_w4_mk_probe(HSD_JObj* j)
{
    static int init;
    static unsigned want;
    static int on;
    unsigned tx;
    if (!init) {
        const char* e = getenv("MSL_W4_MK");
        init = 1;
        if (e != NULL) {
            want = (unsigned) strtoul(e, NULL, 16);
            on = 1;
        }
    }
    if (!on) {
        return;
    }
    memcpy(&tx, &j->translate.x, 4);
    if (tx == want && j->parent != NULL) {
        unsigned w[3], pm[12], mm[12];
        int k;
        extern int msl_w4_cur_step;
        memcpy(w, &j->rotate, 12);
        memcpy(pm, j->parent->mtx, 48);
        memcpy(mm, j->mtx, 48);
        fprintf(stderr, "mk step %d j %p rot %08x %08x %08x par", msl_w4_cur_step, (void*) j, w[0], w[1], w[2]);
        for (k = 0; k < 12; ++k) {
            fprintf(stderr, " %08x", pm[k]);
        }
        fprintf(stderr, " out");
        for (k = 0; k < 12; ++k) {
            fprintf(stderr, " %08x", mm[k]);
        }
        fputc(10, stderr);
        if (getenv("MSL_W4_TRAP") != NULL && msl_w4_cur_step == atoi(getenv("MSL_W4_TRAP")) && (w[0] == 0xbf326529u || w[0] == 0xbf326521u)) {
            msl_w4_gdb_trap(j);
        }
    }
}
#endif

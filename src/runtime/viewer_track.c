#include "runtime/viewer_track.h"

#include "cm/camera.h"
#include "ft/fighter.h"
#include "ft/chara/ftCommon/ftCo_DamageFall.h"
#include "ft/ft_0DF0.h"
#include "ft/ftlib.h"
#include "ft/types.h"
#include "gr/ground.h"
#include "gr/inlines.h"
#include "gr/types.h"
#include "it/inlines.h"
#include "it/itCharItems.h"
#include "it/types.h"
#include "lb/types.h"
#include "runtime/context.h"
#include "runtime/effects.h"
#include "runtime/scalar.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <baselib/gobj.h>
#include <baselib/jobj.h>
#include <baselib/random.h>

// The game camera's render transform and extras (cm/camera.c), read only.
void msl_camera_get_render_transform(Vec3* position, Vec3* interest,
                                     float* fov);
void msl_camera_track_extras(Vec2* translation, u8* background_rgb);

// scalar.c's external stage ids.
enum {
    TRACK_STAGE_BATTLEFIELD = 31,
    TRACK_STAGE_FINAL_DESTINATION = 32,
    TRACK_STAGE_FOUNTAIN = 2,
};

// A state is written only when it changes: the last value of each key (its
// kind and identity fields) is kept.
enum { TRACK_KEYS = 2048, TRACK_KEY_LEN = 48, TRACK_VALUE_LEN = 200 };

struct MslViewerTrack {
    // Lines since the last msl_core_match_viewer_track.
    char* text;
    size_t size;
    size_t capacity;
    struct {
        char key[TRACK_KEY_LEN];
        char value[TRACK_VALUE_LEN];
    } state[TRACK_KEYS];
};

_Thread_local MslViewerTrack* msl_core_context_viewer_track;

MslViewerTrack* msl_viewer_track_create(void)
{
    MslViewerTrack* track = calloc(1, sizeof(*track));
    if (track == NULL) {
        return NULL;
    }
    track->capacity = 64 * 1024;
    track->text = malloc(track->capacity);
    if (track->text == NULL) {
        free(track);
        return NULL;
    }
    track->text[0] = '\0';
    return track;
}

void msl_viewer_track_destroy(MslViewerTrack* track)
{
    if (track != NULL) {
        free(track->text);
        free(track);
    }
}

const char* msl_core_match_viewer_track(MslCoreMatch* match, size_t* size)
{
    MslViewerTrack* track = match != NULL ? match->viewer_track : NULL;
    if (track == NULL) {
        *size = 0;
        return NULL;
    }
    *size = track->size;
    track->size = 0;
    return track->text;
}

static void put(MslViewerTrack* track, const char* format, ...)
{
    for (;;) {
        size_t room = track->capacity - track->size;
        va_list args;
        int n;
        va_start(args, format);
        n = vsnprintf(track->text + track->size, room, format, args);
        va_end(args);
        if (n < 0) {
            track->text[track->size] = '\0';
            return;
        }
        if ((size_t) n < room) {
            track->size += (size_t) n;
            return;
        }
        {
            size_t capacity = track->capacity * 2;
            char* grown;
            while (capacity <= track->size + (size_t) n) {
                capacity *= 2;
            }
            grown = realloc(track->text, capacity);
            if (grown == NULL) {
                fprintf(stderr, "viewer track: out of memory\n");
                abort();
            }
            track->text = grown;
            track->capacity = capacity;
        }
    }
}

// The frame a hook fires in: the match advances frame_id after the gameplay
// procs, so a hook inside a step (or construction) belongs to frame_id + 1.
static int hook_frame(void)
{
    MslCoreMatch* match = msl_core_try_active_match();
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

// Writes [frame,key value] when value differs from the last one for key.
static void emit_state(MslViewerTrack* track, int frame, const char* key,
                       const char* value)
{
    unsigned slot = hash_key(key) % TRACK_KEYS;
    unsigned probe;

    for (probe = 0; probe < TRACK_KEYS; ++probe) {
        unsigned i = (slot + probe) % TRACK_KEYS;
        if (track->state[i].key[0] == '\0') {
            snprintf(track->state[i].key, TRACK_KEY_LEN, "%s", key);
            snprintf(track->state[i].value, TRACK_VALUE_LEN, "%s", value);
            put(track, "[%d,%s%s]\n", frame, key, value);
            return;
        }
        if (strcmp(track->state[i].key, key) == 0) {
            if (strcmp(track->state[i].value, value) != 0) {
                snprintf(track->state[i].value, TRACK_VALUE_LEN, "%s", value);
                put(track, "[%d,%s%s]\n", frame, key, value);
            }
            return;
        }
    }
}

// The last value written for key, or NULL.
static const char* last_state(const MslViewerTrack* track, const char* key)
{
    unsigned slot = hash_key(key) % TRACK_KEYS;
    unsigned probe;

    for (probe = 0; probe < TRACK_KEYS; ++probe) {
        unsigned i = (slot + probe) % TRACK_KEYS;
        if (track->state[i].key[0] == '\0') {
            return NULL;
        }
        if (strcmp(track->state[i].key, key) == 0) {
            return track->state[i].value;
        }
    }
    return NULL;
}

static uint32_t rgba(GXColor c)
{
    return ((uint32_t) c.r << 24) | ((uint32_t) c.g << 16) |
           ((uint32_t) c.b << 8) | (uint32_t) c.a;
}

// The game's random state: the simulator's, stepped once for each draw the
// game has made this frame that the simulator leaves out.
static uint32_t seed_now(const MslViewerTrack* track)
{
    uint32_t state = *seed_ptr;
    (void) track;
    return state;
}

u32 msl_viewer_track_state(void)
{
    return seed_now(msl_core_context_viewer_track);
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

// [frame,"fx",gfx_id,owner kind,owner id,joint,x,y,z,a0,a1,seed]
void msl_viewer_track_fx(HSD_GObj* owner, s32 gfx_id, HSD_JObj* jobj,
                         const Vec3* pos, float a0, float a1)
{
    MslViewerTrack* track = msl_core_context_viewer_track;
    char kind;
    int id;
    int joint;
    Vec3 at = { 0.0F, 0.0F, 0.0F };

    if (track == NULL) {
        return;
    }
    if (pos != NULL) {
        at = *pos;
    }
    describe_owner(owner, jobj, &kind, &id, &joint);
    put(track, "[%d,\"fx\",%d,\"%c\",%d,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%u]\n",
        hook_frame(), (int) gfx_id, kind, id, joint, at.x, at.y, at.z, a0, a1,
        (unsigned) seed_now(track));
}

// [frame,"fxend",owner kind,owner id]: efLib_DestroyAll on the owner.
void msl_viewer_track_fxend(HSD_GObj* owner)
{
    MslViewerTrack* track = msl_core_context_viewer_track;
    char kind;
    int id;
    int joint;

    if (track == NULL) {
        return;
    }
    describe_owner(owner, NULL, &kind, &id, &joint);
    put(track, "[%d,\"fxend\",\"%c\",%d]\n", hook_frame(), kind, id);
}

void msl_viewer_track_tex(HSD_GObj* fighter, int slot, float index)
{
    MslViewerTrack* track = msl_core_context_viewer_track;
    Fighter* fp;

    if (track == NULL || fighter == NULL) {
        return;
    }
    fp = GET_FIGHTER(fighter);
    put(track, "[%d,\"tex\",%d,%d,%d,%.6g]\n", hook_frame(),
        (int) fp->player_id, fp->x221F_b4 ? 1 : 0, slot, index);
}

// [frame,"tint",port,sub,slot,index,rgba,flags,light rgba,light x,light yz]
// while a color overlay runs; ",0" when it stops.
static void track_overlay(MslViewerTrack* track, int frame, int port, int sub,
                          int slot, const ColorOverlay* co)
{
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];
    int index = co->x28_colanim.i;
    uint8_t flags;

    snprintf(key, sizeof(key), "\"tint\",%d,%d,%d", port, sub, slot);
    if (index == 0) {
        const char* last = last_state(track, key);
        if (last == NULL) {
            return;
        }
        emit_state(track, frame, key, ",0");
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
    emit_state(track, frame, key, value);
}

static void track_charge(MslViewerTrack* track, int frame, int port, int sub,
                         const char* field, long value)
{
    char key[TRACK_KEY_LEN];
    char text[32];
    snprintf(key, sizeof(key), "\"charge\",%d,%d,\"%s\"", port, sub, field);
    snprintf(text, sizeof(text), ",%ld", value);
    emit_state(track, frame, key, text);
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

static void track_fighter(MslViewerTrack* track, int frame,
                          Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    int port = fp->player_id;
    int sub = fp->x221F_b4 ? 1 : 0;
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];
    Vec3 shake;
    int i;

    track_overlay(track, frame, port, sub, 0, &fp->x408);
    track_overlay(track, frame, port, sub, 1, &fp->x488);
    track_overlay(track, frame, port, sub, 2, &fp->x508);

    switch (fp->kind) {
    case FTKIND_DONKEY:
        track_charge(track, frame, port, sub, "dk", fp->fv.dk.x222C);
        break;
    case FTKIND_SAMUS:
        track_charge(track, frame, port, sub, "ss", fp->fv.ss.x2230);
        break;
    case FTKIND_MEWTWO:
        track_charge(track, frame, port, sub, "mt",
                     fp->fv.mt.x2234_shadowBallCharge);
        break;
    case FTKIND_SEAK:
        track_charge(track, frame, port, sub, "sk", fp->fv.sk.x0);
        break;
    case FTKIND_GAMEWATCH:
        track_charge(track, frame, port, sub, "gw",
                     fp->fv.gw.x2238_panicCharge);
        break;
    case FTKIND_KIRBY:
        track_charge(track, frame, port, sub, "kb.kind",
                     (long) fp->fv.kb.hat.kind);
        track_charge(track, frame, port, sub, "kb.dk", fp->fv.kb.xBC);
        track_charge(track, frame, port, sub, "kb.ss", fp->fv.kb.xA8);
        break;
    default:
        break;
    }

    // Model part variants as shown (ftParts_80074A4C followed by the apply,
    // or the script's ftParts_80074B0C).
    for (i = 0; i < fp->x5AC.model_num && i < 12; ++i) {
        snprintf(key, sizeof(key), "\"part\",%d,%d,%d", port, sub, i);
        snprintf(value, sizeof(value), ",%d", (int) fp->x5F4_arr[i].idx);
        emit_state(track, frame, key, value);
    }

    // The animation as the game plays it: its index in the subaction table,
    // whose table (the fighter's own kind, or a thrower's), the frame, the
    // rate, and the blend into it (blend frames, and how far in:
    // ftAnim_8006EBA4, ftAnim_8006FE9C).
    snprintf(key, sizeof(key), "\"anim\",%d,%d", port, sub);
    snprintf(value, sizeof(value), ",%d,%d,%.6g,%.6g,%.6g,%.6g",
             (int) fp->anim_id, (int) Fighter_AnimSourceKind(fp),
             fp->cur_anim_frame, fp->frame_speed_mul,
             fp->x8A4_animBlendFrames, fp->x8A8_anim_frame);
    emit_state(track, frame, key, value);

    // DeadUpStar/DeadUpFall (Slippi action states 4-10) or off the plane:
    // the top joint's place, turn and size.
    if ((fp->motion_id >= 4 && fp->motion_id <= 10) || fp->cur_pos.z != 0.0F) {
        HSD_JObj* top = fp->parts != NULL ? fp->parts[FtPart_TopN].joint
                                          : NULL;
        if (top != NULL) {
            snprintf(key, sizeof(key), "\"pos\",%d,%d", port, sub);
            snprintf(value, sizeof(value),
                     ",%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g", top->translate.x,
                     top->translate.y, top->translate.z, top->rotate.x,
                     top->rotate.y, top->rotate.z, top->scale.x);
            emit_state(track, frame, key, value);
        }
    }

    snprintf(key, sizeof(key), "\"shake\",%d,%d", port, sub);
    if (track_shake(fp, &shake)) {
        snprintf(value, sizeof(value), ",%.6g,%.6g,%.6g", shake.x, shake.y,
                 shake.z);
        emit_state(track, frame, key, value);
    } else if (last_state(track, key) != NULL) {
        emit_state(track, frame, key, ",0,0,0");
    }

    snprintf(key, sizeof(key), "\"hide\",%d,%d", port, sub);
    snprintf(value, sizeof(value), ",%d",
             (fp->invisible ? 1 : 0) | (fp->x221E_b5 ? 2 : 0) |
                 (fp->x2226_b5 ? 4 : 0));
    if (strcmp(value, ",0") != 0 || last_state(track, key) != NULL) {
        emit_state(track, frame, key, value);
    }
}

static float round2(float v)
{
    return roundf(v * 100.0F) / 100.0F;
}

static void track_stage(MslViewerTrack* track, int frame,
                        const MslCoreMatch* match)
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
                emit_state(track, frame, "\"bg\"", value);
            } else {
                float frames = gp->u.map.xC4_b27 ? 0.0F : gp->u.map.x104;
                snprintf(value, sizeof(value), ",%d,%.6g,%.6g,%.6g,%.6g",
                         (int) gp->u.map.xC4_b2_25, gp->u.map.xE4,
                         gp->u.map.xE8, gp->u.map.xEC,
                         frames > 0.0F ? frames : 0.0F);
                emit_state(track, frame, "\"fd\"", value);
                // Part 7's root turns while the background is in phases 9
                // and 10 (grLast_8021C40C and its animation).
                if (gp->u.map.xC4_b2_25 == 9 || gp->u.map.xC4_b2_25 == 10) {
                    HSD_GObj* part = Ground_801C2BA4(7);
                    HSD_JObj* root = part != NULL ? part->hsd_obj : NULL;
                    if (root != NULL) {
                        snprintf(value, sizeof(value), ",%.6g,%.6g,%.6g",
                                 root->rotate.x, root->rotate.y,
                                 root->rotate.z);
                        emit_state(track, frame, "\"fdrot\"", value);
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
    if (match->config.stage_id == TRACK_STAGE_FOUNTAIN &&
        match->gobj.entities != NULL)
    {
        float h[2] = { 0.0F, 0.0F };
        int have = 0;
        HSD_GObj* gobj;
        for (gobj = (HSD_GObj*) match->gobj.entities->x14; gobj != NULL;
             gobj = gobj->next)
        {
            Ground* gp;
            if (gobj->classifier != HSD_GOBJ_CLASS_STAGE ||
                gobj->user_data == NULL)
            {
                continue;
            }
            gp = GET_GROUND(gobj);
            if ((int) gp->map_id == 4 &&
                (gp->gv.izumi3.xC8 == 0 || gp->gv.izumi3.xC8 == 1))
            {
                h[gp->gv.izumi3.xC8] = gp->gv.izumi3.xD0;
                have |= 1 << gp->gv.izumi3.xC8;
            }
        }
        if (have == 3) {
            snprintf(value, sizeof(value), ",%.6g,%.6g", h[0], h[1]);
            emit_state(track, frame, "\"fod\"", value);
        }
    }

    // Stage material color scripts (grMaterial_801C9604) while they run.
    for (i = 0; i < 64; ++i) {
        HSD_GObj* gobj = Ground_801C2BA4(i);
        Ground* gp;
        uint32_t color = 0;
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
            color = rgba(c);
        }
        snprintf(key, sizeof(key), "\"gcolor\",%d", (int) gp->map_id);
        snprintf(value, sizeof(value), ",%u", (unsigned) color);
        if (color != 0 || last_state(track, key) != NULL) {
            emit_state(track, frame, key, value);
        }
    }

    msl_camera_get_render_transform(&eye, &interest, &fov);
    msl_camera_track_extras(&translation, bg);
    snprintf(value, sizeof(value), ",%u",
             ((unsigned) bg[0] << 24) | ((unsigned) bg[1] << 16) |
                 ((unsigned) bg[2] << 8) | 0xFFU);
    emit_state(track, frame, "\"bgcolor\"", value);
    // The render transform includes the quake translation; the unshaken
    // place and the translation are written apart.
    snprintf(value, sizeof(value),
             ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
             round2(eye.x - translation.x), round2(eye.y - translation.y),
             round2(eye.z), round2(interest.x - translation.x),
             round2(interest.y - translation.y), round2(interest.z),
             round2(fov), round2(translation.x), round2(translation.y));
    emit_state(track, frame, "\"cam\"", value);
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
// still at the origin (not out yet) are left out, and a chain with none
// left writes no event. Rounded to 0.01.
static void track_chain(MslViewerTrack* track, int frame, Item* ip,
                        const Vec3* head, ItemLink* start, bool along_next,
                        const Vec3* tail)
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
            (link->pos.x == 0.0F && link->pos.y == 0.0F))
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
    put(track, "[%d,%s%s]\n", frame, key, value);
}

static void track_items(MslViewerTrack* track, int frame,
                        const MslCoreMatch* match)
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
                track_chain(track, frame, ip, &hook->pos, hook, true, NULL);
            }
            break;
        }
        case It_Kind_Samus_GBeam: {
            // The claw is the last link (xDD4+0), which the grapple moves and
            // collides; the other links follow joint animation the headless
            // core does not run, so only the ones that have moved appear.
            ItemLink* claw = ip->xDD4_itemVar.samusgrapple.x0;
            if (claw != NULL) {
                track_chain(track, frame, ip, &claw->pos, claw, true, NULL);
            }
            break;
        }
        case It_Kind_Ness_Yoyo: {
            // Ness's yo-yo: the string's links from the hand (xDD4+C) toward
            // the ball, then the ball (xDD4+8, the link the yo-yo's hitbox
            // position is written to) last. The item itself stays at its
            // spawn point.
            ItemLink* ball = ip->xDD4_itemVar.nessyoyo.x8;
            track_chain(track, frame, ip, NULL, ip->xDD4_itemVar.nessyoyo.xC,
                        false, ball != NULL ? &ball->pos : NULL);
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
        if (last_state(track, key) == NULL) {
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
            emit_state(track, frame, key, value);
        }
        // The root joint's Euler rotation, on change (spinning throws,
        // turnips, bombs).
        if (jobj != NULL && !(jobj->flags & JOBJ_USE_QUATERNION)) {
            snprintf(key, sizeof(key), "\"irot\",%d", (int) ip->x1C);
            snprintf(value, sizeof(value), ",%.4f,%.4f,%.4f", jobj->rotate.x,
                     jobj->rotate.y, jobj->rotate.z);
            if (last_state(track, key) != NULL || jobj->rotate.x != 0.0F ||
                jobj->rotate.y != 0.0F || jobj->rotate.z != 0.0F)
            {
                emit_state(track, frame, key, value);
            }
        }
        gobj = (Item_GObj*) gobj->next;
    }
}

void msl_viewer_track_frame(MslCoreMatch* match)
{
    MslViewerTrack* track = match->viewer_track;
    int frame;
    int i;

    if (track == NULL) {
        return;
    }
    frame = match->frame_id;
    for (i = 0; i < match->config.num_players; ++i) {
        if (match->fighters[i] != NULL) {
            track_fighter(track, frame, match->fighters[i]);
        }
        if (match->follower_fighters[i] != NULL &&
            !GET_FIGHTER(match->follower_fighters[i])->x221F_b3)
        {
            track_fighter(track, frame, match->follower_fighters[i]);
        }
    }
    track_stage(track, frame, match);
    track_items(track, frame, match);
}

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
#include "runtime/effect_pieces.h"
#include "runtime/scalar.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <baselib/gobj.h>
#include <baselib/gobjproc.h>
#include <baselib/jobj.h>
#include <baselib/random.h>

// The game camera's render transform (cm/camera.c), read only.
void msl_camera_get_render_transform(Vec3* position, Vec3* interest,
                                     float* fov);
void msl_camera_track_extras(Vec2* translation, u8* background_rgb);

int msl_viewer_track_on;

// scalar.c's external stage ids.
enum {
    TRACK_STAGE_BATTLEFIELD = 31,
    TRACK_STAGE_FINAL_DESTINATION = 32,
};

static FILE* track_out;

// Last value written for each state key, so a state is written only when it
// changes. Keys are the type and identity fields of the line.
enum { TRACK_KEYS = 2048, TRACK_KEY_LEN = 48, TRACK_VALUE_LEN = 200 };
static struct {
    char key[TRACK_KEY_LEN];
    char value[TRACK_VALUE_LEN];
} track_state[TRACK_KEYS];

void msl_viewer_track_begin(FILE* out)
{
    track_out = out;
    msl_viewer_track_on = out != NULL;
    memset(track_state, 0, sizeof(track_state));
}

void msl_viewer_track_end(void)
{
    if (track_out != NULL) {
        fflush(track_out);
    }
    track_out = NULL;
    msl_viewer_track_on = 0;
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

static uint32_t seed_now(void)
{
    return *seed_ptr;
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
//  tx,ty,tz,rx,ry,rz,sx,sy,sz,callback,lifetime,p0,p1,p2,mirrored]
void msl_viewer_track_fxp(HSD_GObj* owner, s32 gfx_id,
                          const MslEffectPiece* p)
{
    char kind;
    int id;
    int joint;

    if (track_out == NULL) {
        return;
    }
    describe_owner(owner, p->joint, &kind, &id, &joint);
    fprintf(track_out,
            "[%d,\"fxp\",%d,\"%c\",%d,%d,\"%c\",%d,\"%s\",%d,"
            "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,\"%s\",%d,"
            "%.6g,%.6g,%.6g,%d]\n",
            hook_frame(), (int) gfx_id, kind, id, p->index, p->kind,
            (int) p->id, p->how, joint, p->t.x, p->t.y, p->t.z, p->r.x,
            p->r.y, p->r.z, p->s.x, p->s.y, p->s.z, p->callback,
            p->lifetime, p->params.x, p->params.y, p->params.z,
            p->mirrored);
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
}

void msl_viewer_track_gen(int bank, int gen_id, HSD_JObj* jobj)
{
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
    fprintf(track_out, "[%d,\"gen\",%d,%d,%.6g,%.6g,%.6g,%u]\n",
            hook_frame(), bank, gen_id, x, y, z, (unsigned) seed_now());
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

void msl_viewer_track_sgen(int bank, int gen_id, const Vec3* pos,
                           HSD_JObj* jobj)
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
    fprintf(track_out, "[%d,\"sgen\",%d,%d,%d,%.6g,%.6g,%.6g,%u]\n",
            hook_frame(), running_map_id(), bank, gen_id, at.x, at.y, at.z,
            (unsigned) seed_now());
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

    // DeadUpStar/DeadUpFall (Slippi action states 4-10) or off the plane.
    if ((fp->motion_id >= 4 && fp->motion_id <= 10) || fp->cur_pos.z != 0.0F) {
        HSD_JObj* top = fp->parts != NULL ? fp->parts[FtPart_TopN].joint
                                          : NULL;
        if (top != NULL) {
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
// still at the origin (not out yet) are left out, and a chain with none
// left writes no event. Rounded to 0.01.
static void track_chain(int frame, Item* ip, const Vec3* head,
                        ItemLink* start, bool along_next, const Vec3* tail)
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
                track_chain(frame, ip, &hook->pos, hook, true, NULL);
            }
            break;
        }
        case It_Kind_Samus_GBeam: {
            // The claw is the last link (xDD4+0), which the grapple moves and
            // collides; the other links follow joint animation the headless
            // core does not run, so only the ones that have moved appear.
            ItemLink* claw = ip->xDD4_itemVar.samusgrapple.x0;
            if (claw != NULL) {
                track_chain(frame, ip, &claw->pos, claw, true, NULL);
            }
            break;
        }
        case It_Kind_Ness_Yoyo: {
            // Ness's yo-yo: the string's links from the hand (xDD4+C) toward
            // the ball, then the ball (xDD4+8, the link the yo-yo's hitbox
            // position is written to) last. The item itself stays at its
            // spawn point.
            ItemLink* ball = ip->xDD4_itemVar.nessyoyo.x8;
            track_chain(frame, ip, NULL, ip->xDD4_itemVar.nessyoyo.xC, false,
                        ball != NULL ? &ball->pos : NULL);
            break;
        }
        default:
            break;
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

void msl_viewer_track_frame(MslCoreMatch* match)
{
    int frame;
    int i;

    if (track_out == NULL) {
        return;
    }
    msl_core_bind_match(match);
    frame = match->frame_id;
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
}

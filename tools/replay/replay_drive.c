/* Drive one match from a recorded tape: the replay lane, outside validation.
 *
 * The public melee_sim API cannot replay a recording. It hardcodes the match's
 * start frame (api.c: frame_id = -123, where a recording's first finalized row
 * needs first - 1) and has no way to pass the per-frame RNG Slippi records, so
 * a recording diverges in the entry animation and never comes back.
 *
 * The core already has what is needed: msl_core_match_step() takes the frame
 * seed and the stage events, which is how tools/validation/native.c reproduces
 * 512 recordings exactly. This is that loop and nothing else, reading a tape
 * simple enough to write from Python without mirroring any packed struct.
 *
 * A caller that wants a position out of somebody's real game - to start an
 * agent there, say - replays the recording forward to that frame and compares.
 *
 *   replay_drive --sizes
 *   replay_drive <data_root> <tape> <out> [--run <lanes>] [--save <state>]
 *   replay_drive <data_root> <tape> <out> --load <state> --run <lanes>
 *   replay_drive <data_root> --serve
 *
 * --save writes the match as it stands at the end of the tape (the position
 * run mode forks its lanes from) to <state>, a core savestate. --load starts
 * from such a state instead of replaying the tape: the tape is read only for
 * the match's settings and its first frame's inputs, and <out> is not
 * written. A state is good only for the build that saved it (the savestate
 * header checks the layout, the game data and the image); a state this build
 * cannot take is an error, exit code 3, so the caller can replay instead.
 *
 * --serve keeps one driver for many runs, the game data read once (most of
 * a driver's start): each run is a line "run <groups>" and a line a group,
 * "<lanes>\t<tape>\t<state>\t<out>" (state and out may be "-"). A group's
 * position is its state when this build can take it, else its tape replayed
 * (its rows written to <out>, the state saved to <state>). The run's lanes,
 * every group's in order, answer as run mode's do; then each step is a byte
 * 'S' and the lanes' inputs, and 'E' ends the run. "quit" or the end of the
 * input ends the driver.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/item_projection.h"
#include "runtime/savestate.h"
#include "runtime/scalar.h"
#include "runtime/wire.h"
#include "runtime/viewer_track.h"
#if MSL_GX_RECORD
#include "gx/msl_gx.h"
#endif

#if defined(_WIN32)
/* Run mode trades packed structs over the standard streams, and Windows
   opens those in text mode: every 0x0A byte of a float would go out as
   0x0D 0x0A. */
#include <fcntl.h>
#include <io.h>
#define MSL_BINARY_STREAMS()                                                  \
    do {                                                                      \
        _setmode(_fileno(stdin), _O_BINARY);                                  \
        _setmode(_fileno(stdout), _O_BINARY);                                 \
    } while (0)
#else
#define MSL_BINARY_STREAMS() ((void) 0)
#endif

#pragma pack(push, 1)

typedef struct TapePlayer {
    uint8_t char_id;
    uint8_t team_id;
    uint8_t facing_and_port;
    uint8_t costume_id;
    uint8_t handicap;
    uint8_t start_percent;
    uint8_t cpu_level;
    uint8_t pad;
} TapePlayer;

typedef struct TapeConfig {
    uint32_t magic;
    int32_t stage_id;
    int32_t frame_id;
    uint32_t frame_seed;
    uint32_t initial_seed;
    float damage_ratio;
    uint8_t num_players;
    uint8_t is_teams;
    uint8_t friendly_fire;
    uint8_t stock_count;
    uint8_t online_fnmsubs_zero;
    uint8_t brawl_offscreen_damage;
    uint8_t freeze_dead_up_fall_physics;
    uint8_t whispy_dead_fighter_fix;
    uint8_t ucf_cardinals_1_0_enabled;
    uint8_t ucf_shield_sdi_enabled;
    uint8_t ucf_sdi_enabled;
    uint8_t ucf_shield_drop_extended_enabled;
    uint8_t ucf_shield_drop_084_enabled;
    uint8_t stage_event_streams;
    uint8_t stadium_transformations;    /* MSL_STADIUM_* (wire.h) */
    uint8_t slippi_patches;             /* MSL_PATCH_* (wire.h) */
    uint8_t jit_arithmetic;             /* MSL_JIT_* (wire.h) */
    TapePlayer players[4];
    uint32_t frame_count;
} TapeConfig;

typedef struct TapeInput {
    uint16_t buttons;
    int8_t main_x;
    int8_t main_y;
    int8_t c_x;
    int8_t c_y;
    uint8_t l;
    uint8_t r;
    uint8_t nml_valid;
    float nml_main_x;
    float nml_main_y;
    float nml_c_x;
    float nml_c_y;
} TapeInput;

typedef struct TapeFrame {
    uint32_t frame_seed;
    uint32_t fighter_seed;
    uint8_t fighter_seed_valid;
    uint8_t fod_valid;
    uint8_t whispy_valid;
    uint8_t whispy_direction;
    float fod_left;
    float fod_right;
    TapeInput p[4];
} TapeFrame;

/* One item as Slippi's item event records it. misc_mask says which of the
   four misc bytes are gameplay state for this kind and state
   (item_projection.h); the others are written as zero, and a comparison
   skips them. */
typedef struct OutItem {
    uint8_t exists;
    uint8_t state;
    int8_t owner;
    uint8_t misc_mask;
    uint16_t type;
    uint16_t instance_id;
    uint8_t misc[4];
    uint16_t damage;
    uint16_t pad;
    uint32_t spawn_id;
    float direction;
    float vel_x;
    float vel_y;
    float pos_x;
    float pos_y;
    float timer;
} OutItem;

typedef struct OutFrame {
    int32_t frame_id;
    uint32_t frame_seed;
    float pos_x[4];
    float pos_y[4];
    float percent[4];
    float shield_hp[4];
    uint16_t action_id[4];
    int16_t action_frame[4];
    uint16_t hitstun[4];
    uint16_t hitlag[4];
    uint8_t stocks[4];
    uint8_t facing[4];
    uint8_t on_ground[4];
    uint8_t is_dead[4];
    uint8_t jumps_left[4];
    uint8_t hurtbox_state[4];
    uint8_t last_attack_landed[4];
    uint8_t combo_count[4];
    /* Ice Climbers' Nana, per port. Slippi records her as the port's
       follower, with rows only while she is awake; follower_present is that
       life cycle, and the other lanes are zero while she is absent. */
    uint8_t follower_present[4];
    uint8_t follower_facing[4];
    uint8_t follower_stocks[4];
    uint8_t item_count;
    uint8_t pad[3];
    /* Pokemon Stadium's transformation phase and kind (5 = home). */
    uint16_t stadium_phase;
    uint16_t stadium_kind;
    uint16_t follower_action_id[4];
    float follower_pos_x[4];
    float follower_pos_y[4];
    float follower_percent[4];
    float follower_shield_hp[4];
    OutItem items[MSL_CORE_MAX_ITEMS];
} OutFrame;

/* Run mode. Once the tape has been replayed, the caller can keep stepping the
   match with inputs of its own - an agent playing on from the position the
   recording reached. Every lane starts from the same saved state, so several
   runs of one position play at once, and the rows below carry everything a
   Slippi post-frame event needs so the caller can write the run out as a
   replay of its own. */
typedef struct RunPlayer {
    float pos_x;
    float pos_y;
    float percent;
    float shield_hp;
    float speed_air_x;
    float speed_ground_x;
    float speed_y;
    float speed_x_attack;
    float speed_y_attack;
    uint32_t animation_index;
    uint16_t action_id;
    int16_t action_frame;
    uint16_t hitlag;
    uint16_t hitstun;
    uint16_t ground_id;
    uint16_t instance_hit_by;
    uint16_t instance_id;
    uint8_t char_id;
    uint8_t stocks;
    uint8_t facing;
    uint8_t on_ground;
    uint8_t is_dead;
    uint8_t jumps_left;
    uint8_t hurtbox_state;
    uint8_t l_cancel;
    uint8_t last_attack_landed;
    uint8_t combo_count;
    uint8_t last_hit_by;
    uint8_t state_flags[5];
} RunPlayer;

typedef struct RunOut {
    int32_t frame_id;
    uint32_t frame_seed;
    RunPlayer p[4];
} RunOut;

typedef struct RunIn {
    TapeInput p[4];
} RunIn;

#pragma pack(pop)

/* tools/validation/native.c::processed_stick_i8 */
static int8_t processed_stick_i8(float value)
{
    if (value <= -1.0F) {
        return -80;
    }
    if (value >= 1.0F) {
        return 80;
    }
    return (int8_t) (value * 80.0F);
}

static void fill_input(MslCoreInput* input, const TapeFrame* frame, int count)
{
    int player;
    memset(input, 0, sizeof(*input));
    for (player = 0; player < count; ++player) {
        const TapeInput* src = &frame->p[player];
        MslCoreInputPlayer* dst = &input->p[player];
        dst->buttons = src->buttons;
        dst->main_x = src->main_x;
        dst->main_y = src->main_y;
        dst->c_x = src->c_x;
        dst->c_y = src->c_y;
        dst->l = src->l;
        dst->r = src->r;
        /* Slippi playback restores the post-clamp HSD axes to fighter input
           state separately from the physical bytes; native.c does the same,
           with this truncating conversion. */
        dst->nml_main_x = processed_stick_i8(src->nml_main_x);
        dst->nml_main_y = processed_stick_i8(src->nml_main_y);
        dst->nml_c_x = processed_stick_i8(src->nml_c_x);
        dst->nml_c_y = processed_stick_i8(src->nml_c_y);
        dst->nml_valid = src->nml_valid;
    }
}

static void fill_events(MslCoreStageEvents* events, const TapeFrame* frame)
{
    memset(events, 0, sizeof(*events));
    events->fighter_pre_random_seed = frame->fighter_seed;
    events->fighter_pre_random_seed_valid = frame->fighter_seed_valid;
    if (frame->fod_valid) {
        events->fod_platform_height[0] = frame->fod_left;
        events->fod_platform_height[1] = frame->fod_right;
        events->fod_platform_mask = frame->fod_valid;
    }
    events->dreamland_whispy_valid = frame->whispy_valid;
    events->dreamland_whispy_direction = frame->whispy_direction;
}

static void fill_output(OutFrame* out, const MslCoreCompare* compare)
{
    int i;
    memset(out, 0, sizeof(*out));
    out->frame_id = compare->frame_id;
    out->frame_seed = compare->frame_pre_random_seed;
    for (i = 0; i < 4; ++i) {
        out->pos_x[i] = compare->pos_x[i];
        out->pos_y[i] = compare->pos_y[i];
        out->percent[i] = compare->percent[i];
        out->shield_hp[i] = compare->shield_hp[i];
        out->action_id[i] = compare->action_id[i];
        out->action_frame[i] = compare->action_frame[i];
        out->hitstun[i] = compare->hitstun[i];
        out->hitlag[i] = compare->hitlag[i];
        out->stocks[i] = compare->stocks[i];
        out->facing[i] = compare->facing[i];
        out->on_ground[i] = compare->on_ground[i];
        out->is_dead[i] = compare->is_dead[i];
        out->jumps_left[i] = compare->jumps_left[i];
        out->hurtbox_state[i] = compare->hurtbox_state[i];
        out->last_attack_landed[i] = compare->last_attack_landed[i];
        out->combo_count[i] = compare->combo_count[i];
        out->follower_present[i] = compare->follower_present[i];
        out->follower_facing[i] = compare->follower_facing[i];
        out->follower_stocks[i] = compare->follower_stocks[i];
        out->follower_action_id[i] = compare->follower_action_id[i];
        out->follower_pos_x[i] = compare->follower_pos_x[i];
        out->follower_pos_y[i] = compare->follower_pos_y[i];
        out->follower_percent[i] = compare->follower_percent[i];
        out->follower_shield_hp[i] = compare->follower_shield_hp[i];
    }
    for (i = 0; i < MSL_CORE_MAX_ITEMS; ++i) {
        const MslCoreItem* src = &compare->items[i];
        OutItem* dst = &out->items[i];
        if (!src->exists) {
            continue;
        }
        out->item_count += 1;
        dst->exists = 1;
        dst->state = src->state;
        dst->owner = src->owner;
        dst->misc_mask =
            msl_core_item_gameplay_misc_mask(src->type, src->state);
        dst->type = src->type;
        dst->instance_id = src->instance_id;
        /* The other misc bytes can hold pointers or pool residue, which
           differ run to run; zero them so a row is the same every time, as
           native.c::fingerprint_actual_output does. */
        dst->misc[0] = (dst->misc_mask & 1) ? src->misc0 : 0;
        dst->misc[1] = (dst->misc_mask & 2) ? src->misc1 : 0;
        dst->misc[2] = (dst->misc_mask & 4) ? src->misc2 : 0;
        dst->misc[3] = (dst->misc_mask & 8) ? src->misc3 : 0;
        dst->damage = src->damage;
        dst->spawn_id = src->spawn_id;
        dst->direction = src->direction;
        dst->vel_x = src->vel_x;
        dst->vel_y = src->vel_y;
        dst->pos_x = src->pos_x;
        dst->pos_y = src->pos_y;
        dst->timer = src->timer;
    }
}

static void fill_run(RunOut* out, const MslCoreCompare* compare)
{
    int i;
    memset(out, 0, sizeof(*out));
    out->frame_id = compare->frame_id;
    out->frame_seed = compare->frame_pre_random_seed;
    for (i = 0; i < 4; ++i) {
        RunPlayer* p = &out->p[i];
        p->pos_x = compare->pos_x[i];
        p->pos_y = compare->pos_y[i];
        p->percent = compare->percent[i];
        p->shield_hp = compare->shield_hp[i];
        p->speed_air_x = compare->speed_air_x_self[i];
        p->speed_ground_x = compare->speed_ground_x_self[i];
        p->speed_y = compare->speed_y_self[i];
        p->speed_x_attack = compare->speed_x_attack[i];
        p->speed_y_attack = compare->speed_y_attack[i];
        p->animation_index = compare->animation_index[i];
        p->action_id = compare->action_id[i];
        p->action_frame = compare->action_frame[i];
        p->hitlag = compare->hitlag[i];
        p->hitstun = compare->hitstun[i];
        p->ground_id = compare->ground_id[i];
        p->instance_hit_by = compare->instance_hit_by[i];
        p->instance_id = compare->instance_id[i];
        p->char_id = compare->char_id[i];
        p->stocks = compare->stocks[i];
        p->facing = compare->facing[i];
        p->on_ground = compare->on_ground[i];
        p->is_dead = compare->is_dead[i];
        p->jumps_left = compare->jumps_left[i];
        p->hurtbox_state = compare->hurtbox_state[i];
        p->l_cancel = compare->l_cancel[i];
        p->last_attack_landed = compare->last_attack_landed[i];
        p->combo_count = compare->combo_count[i];
        p->last_hit_by = compare->last_hit_by[i];
        memcpy(p->state_flags, compare->state_flags[i],
               MSL_CORE_STATE_FLAGS_BYTES);
    }
}

/* The most lanes run mode takes. The lanes live on the heap, so this is a
   sanity bound rather than the size of anything: one lane's match is about
   1 MB, so this many is a few GB - more than any caller has wanted (a few
   hundred runs of one position). `--sizes` reports it. */
enum { MAX_LANES = 4096 };

/* Play on from the replayed position, one lane per run, until the caller
   stops sending inputs. Every lane is the same saved state to start with, so
   the runs differ only by what their agents press. */
static int run_mode(MslCoreMatch* replayed, const MslCoreGameData* game_data,
                    const MslCoreInput* previous, int lanes)
{
    static uint8_t state[1 << 21];
    MslCoreMatch* storage;
    MslCoreMatch** matches;
    RunOut* out;
    RunIn* in;
    size_t needed;
    size_t written;
    int lane;

    if (lanes < 1 || lanes > MAX_LANES) {
        fprintf(stderr, "lanes must be 1..%d\n", MAX_LANES);
        return 1;
    }
    /* Lane 0 is the replayed match itself, so storage[0] is never used; it is
       allocated anyway so a lane's index is its own everywhere. */
    storage = (MslCoreMatch*) calloc((size_t) lanes, sizeof(*storage));
    matches = (MslCoreMatch**) calloc((size_t) lanes, sizeof(*matches));
    out = (RunOut*) calloc((size_t) lanes, sizeof(*out));
    in = (RunIn*) calloc((size_t) lanes, sizeof(*in));
    if (storage == NULL || matches == NULL || out == NULL || in == NULL) {
        fprintf(stderr, "not enough memory for %d lanes\n", lanes);
        return 1;
    }
    MSL_BINARY_STREAMS();
    needed = msl_core_match_save_size(replayed);
    if (needed > sizeof(state)) {
        fprintf(stderr, "savestate is %zu bytes, buffer is %zu\n", needed,
                sizeof(state));
        return 1;
    }
    if (msl_core_match_save(replayed, state, sizeof(state), &written) != 0) {
        fprintf(stderr, "save failed\n");
        return 1;
    }
    /* Lane 0 is the replayed match itself. A MslCoreMatch carries pointers
       into its own storage, so it cannot be copied with an assignment: the
       other lanes are built and then given the saved state. */
    matches[0] = replayed;
    for (lane = 1; lane < lanes; ++lane) {
        matches[lane] = &storage[lane];
        /* match_init does its own storage_init, and restore needs a
           destination that already carries the game data and an arena. */
        if (msl_core_match_init(matches[lane], game_data, &replayed->config,
                                previous) != 0)
        {
            fprintf(stderr, "lane %d could not be created\n", lane);
            return 1;
        }
        if (msl_core_match_restore(matches[lane], state, written) != 0) {
            fprintf(stderr, "lane %d could not take the saved state "
                    "(needed %zu, written %zu)\n", lane, needed, written);
            return 1;
        }
    }
    /* The position every lane starts from, before any agent has pressed
       anything. */
    for (lane = 0; lane < lanes; ++lane) {
        fill_run(&out[lane], msl_core_match_output(matches[lane]));
    }
    if (fwrite(out, sizeof(*out), (size_t) lanes, stdout) !=
            (size_t) lanes || fflush(stdout) != 0)
    {
        return 1;
    }
    for (;;) {
        static const MslCoreStageEvents no_events = { 0 };
        size_t got = fread(in, sizeof(*in), (size_t) lanes, stdin);
        if (got != (size_t) lanes) {
            return feof(stdin) ? 0 : 1;           /* the caller is done */
        }
        for (lane = 0; lane < lanes; ++lane) {
            MslCoreInput input;
            TapeFrame carrier;
            memset(&carrier, 0, sizeof(carrier));
            memcpy(carrier.p, in[lane].p, sizeof(carrier.p));
            fill_input(&input, &carrier, matches[lane]->config.num_players);
            if (msl_core_match_step(matches[lane], &input,
                                    matches[lane]->random_seed,
                                    &no_events) != 0)
            {
                fprintf(stderr, "run step failed on lane %d\n", lane);
                return 1;
            }
            fill_run(&out[lane], msl_core_match_output(matches[lane]));
        }
        if (fwrite(out, sizeof(*out), (size_t) lanes, stdout) !=
                (size_t) lanes || fflush(stdout) != 0)
        {
            return 1;
        }
    }
}

/* A tape's settings as the core's match config. */
static void config_from_tape(MslCoreMatchConfig* config, const TapeConfig* tape)
{
    int player;
    memset(config, 0, sizeof(*config));
    config->stage_id = (uint32_t) tape->stage_id;
    config->frame_id = tape->frame_id;
    config->frame_pre_random_seed = tape->frame_seed;
    config->initial_random_seed = tape->initial_seed;
    config->match_damage_ratio = tape->damage_ratio;
    config->num_players = tape->num_players;
    config->is_teams = tape->is_teams;
    config->friendly_fire = tape->friendly_fire;
    config->stock_count = tape->stock_count;
    config->online_fnmsubs_zero = tape->online_fnmsubs_zero;
    config->brawl_offscreen_damage = tape->brawl_offscreen_damage;
    config->freeze_dead_up_fall_physics = tape->freeze_dead_up_fall_physics;
    config->whispy_dead_fighter_fix = tape->whispy_dead_fighter_fix;
    config->ucf_cardinals_1_0_enabled = tape->ucf_cardinals_1_0_enabled;
    config->ucf_shield_sdi_enabled = tape->ucf_shield_sdi_enabled;
    config->ucf_sdi_enabled = tape->ucf_sdi_enabled;
    config->ucf_shield_drop_extended_enabled =
        tape->ucf_shield_drop_extended_enabled;
    config->ucf_shield_drop_084_enabled = tape->ucf_shield_drop_084_enabled;
    config->stage_event_streams = tape->stage_event_streams;
    config->stadium_transformations = tape->stadium_transformations;
    config->slippi_patches = tape->slippi_patches;
    config->jit_arithmetic = tape->jit_arithmetic;
    for (player = 0; player < 4; ++player) {
        config->players[player].char_id = tape->players[player].char_id;
        config->players[player].team_id = tape->players[player].team_id;
        config->players[player].facing_and_port =
            tape->players[player].facing_and_port;
        config->players[player].costume_id = tape->players[player].costume_id;
        config->players[player].handicap = tape->players[player].handicap;
        config->players[player].start_percent =
            tape->players[player].start_percent;
        config->players[player].cpu_level = tape->players[player].cpu_level;
    }
}

#if MSL_GX_RECORD
/* Viewer build: the replay's name tags, as Slippi playback restores them (present/hud.c msl_present_set_nametag). They ride after
 * the tape's last frame: four players x (slot id, 16 text bytes) and "NTG1"; the tape is exactly that much longer. */
extern void msl_present_clear_nametags(void);
extern void msl_present_set_nametag(int player_index, int slot_id, const unsigned char* text16);
extern void msl_present_clear_display_names(void);
extern void msl_present_set_display_name(int port, const unsigned char* sjis31);
extern void msl_present_set_match_seconds(int seconds);
static void restore_nametags(const char* tape_path, const TapeConfig* tape)
{
    /* after the last frame: optionally the name tags (4 x (slot id, 16 text bytes), "NTG1"), then optionally the Slippi display names
     * (4 x 31 Shift JIS bytes, "DNM1"; Playback/Core/RestoreGameInfo.asm PDB_DISPLAY_NAMES) */
    FILE* f = fopen(tape_path, "rb");
    unsigned char block[4 * 31 + 4];
    long end;
    long base = (long) (sizeof(TapeConfig) + (size_t) tape->frame_count * sizeof(TapeFrame));
    long cursor;
    int i;
    msl_present_clear_nametags();
    msl_present_clear_display_names();
    if (f == NULL) {
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    end = ftell(f);
    cursor = end;
    /* last of all, optionally the match's starting timer (u32 seconds, "RUL1"): the Game Start event's timer, which the HUD shows */
    msl_present_set_match_seconds(0);
    if (cursor - base >= 8 && fseek(f, cursor - 8, SEEK_SET) == 0 && fread(block, 1, 8, f) == 8 && memcmp(block + 4, "RUL1", 4) == 0) {
        msl_present_set_match_seconds((int) (((unsigned) block[0] << 24) | ((unsigned) block[1] << 16) | ((unsigned) block[2] << 8) | block[3]));
        cursor -= 8;
    }
    if (cursor - base >= (long) sizeof(block) && fseek(f, cursor - (long) sizeof(block), SEEK_SET) == 0 &&
        fread(block, 1, sizeof(block), f) == sizeof(block) && memcmp(block + 4 * 31, "DNM1", 4) == 0)
    {
        for (i = 0; i < 4; i++) {
            msl_present_set_display_name(i, block + i * 31);
        }
        cursor -= (long) sizeof(block);
    }
    if (cursor - base == 4 * 17 + 4 && fseek(f, cursor - (4 * 17 + 4), SEEK_SET) == 0 &&
        fread(block, 1, 4 * 17 + 4, f) == 4 * 17 + 4 && memcmp(block + 4 * 17, "NTG1", 4) == 0)
    {
        for (i = 0; i < 4; i++) {
            msl_present_set_nametag(i, block[i * 17], block + i * 17 + 1);
        }
    }
    fclose(f);
}
#endif

/* The whole of a file, or NULL. */
static uint8_t* read_whole(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    uint8_t* bytes;
    long n;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    bytes = (uint8_t*) malloc(n > 0 ? (size_t) n : 1);
    if (bytes == NULL || fread(bytes, 1, (size_t) n, f) != (size_t) n) {
        free(bytes);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t) n;
    return bytes;
}

/* The match's savestate written to `path` (via a temporary name, so a reader
   never sees half of one). */
static int save_state(const MslCoreMatch* match, const char* path)
{
    size_t needed = msl_core_match_save_size(match);
    size_t written = 0;
    uint8_t* state = (uint8_t*) malloc(needed > 0 ? needed : 1);
    char part[4096];
    FILE* f;
    int ok;
    if (state == NULL ||
        msl_core_match_save(match, state, needed, &written) != 0 ||
        snprintf(part, sizeof(part), "%s.part", path) >= (int) sizeof(part))
    {
        free(state);
        return -1;
    }
    f = fopen(part, "wb");
    ok = f != NULL && fwrite(state, 1, written, f) == written;
    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }
    free(state);
    remove(path);
    if (!ok || rename(part, path) != 0) {
        remove(part);
        return -1;
    }
    return 0;
}

/* A run's position for --serve: the state when this build takes it, else
   the tape replayed (rows to out_path, the state saved to state_path).
   NULL on failure (said on stderr). */
static MslCoreMatch* serve_position(const MslCoreGameData* game_data,
                                    const char* tape_path,
                                    const char* state_path,
                                    const char* out_path,
                                    MslCoreInput* previous)
{
    TapeConfig tape;
    TapeFrame frame;
    MslCoreMatchConfig config;
    MslCoreMatch* match;
    FILE* input = fopen(tape_path, "rb");
    FILE* output = NULL;
    uint32_t index;

    if (input == NULL || fread(&tape, 1, sizeof(tape), input) != sizeof(tape) ||
        tape.magic != 0x314C534DU ||
        fread(&frame, 1, sizeof(frame), input) != sizeof(frame))
    {
        fprintf(stderr, "serve: cannot read tape %s\n", tape_path);
        if (input != NULL) {
            fclose(input);
        }
        return NULL;
    }
    config_from_tape(&config, &tape);
#if MSL_GX_RECORD
    restore_nametags(tape_path, &tape);
#endif
    fill_input(previous, &frame, tape.num_players);
    match = (MslCoreMatch*) calloc(1, sizeof(*match));
    if (match == NULL || msl_core_match_storage_init(match) != 0 ||
        msl_core_match_reset(match, game_data, &config, previous) != 0)
    {
        fprintf(stderr, "serve: match reset failed\n");
        fclose(input);
        free(match);
        return NULL;
    }
    if (state_path != NULL) {
        size_t size = 0;
        uint8_t* state = read_whole(state_path, &size);
        if (state != NULL) {
            int ok = msl_core_match_restore(match, state, size) == 0;
            free(state);
            if (ok) {
                fclose(input);
                return match;
            }
            /* not this build's: replayed below, from a fresh match */
            msl_core_match_destroy(match);
            memset(match, 0, sizeof(*match));
            if (msl_core_match_storage_init(match) != 0 ||
                msl_core_match_reset(match, game_data, &config, previous) != 0)
            {
                fprintf(stderr, "serve: match reset failed\n");
                fclose(input);
                free(match);
                return NULL;
            }
        }
    }
    if (out_path != NULL) {
        output = fopen(out_path, "wb");
    }
    for (index = 0; index < tape.frame_count; ++index) {
        MslCoreInput step_input;
        MslCoreStageEvents events;
        if (index > 0 &&
            fread(&frame, 1, sizeof(frame), input) != sizeof(frame))
        {
            fprintf(stderr, "serve: short tape at frame %u\n", index);
            break;
        }
        fill_input(&step_input, &frame, tape.num_players);
        fill_events(&events, &frame);
        if (msl_core_match_step(match, &step_input, frame.frame_seed,
                                &events) != 0)
        {
            fprintf(stderr, "serve: step failed at frame %u\n", index);
            fclose(input);
            if (output != NULL) {
                fclose(output);
            }
            msl_core_match_destroy(match);
            free(match);
            return NULL;
        }
        if (output != NULL) {
            OutFrame out;
            int phase;
            int kind;
            fill_output(&out, msl_core_match_output(match));
            msl_core_stadium_state(match, &phase, &kind);
            out.stadium_phase = (uint16_t) phase;
            out.stadium_kind = (uint16_t) kind;
            fwrite(&out, 1, sizeof(out), output);
        }
    }
    fclose(input);
    if (output != NULL) {
        fclose(output);
    }
    if (state_path != NULL) {
        save_state(match, state_path);  /* a later run's start; none is fine */
    }
    return match;
}

/* "-" or nothing: no path. The line's own text otherwise (it is cut in place). */
static char* serve_field(char** cursor)
{
    char* start = *cursor;
    char* end;
    if (start == NULL) {
        return NULL;
    }
    end = strchr(start, '\t');
    if (end != NULL) {
        *end = '\0';
        *cursor = end + 1;
    } else {
        *cursor = NULL;
        end = start + strlen(start);
        while (end > start && (end[-1] == '\n' || end[-1] == '\r')) {
            *--end = '\0';
        }
    }
    return (start[0] == '\0' || strcmp(start, "-") == 0) ? NULL : start;
}

enum { SERVE_GROUPS_MAX = 512 };

static int serve_mode(const MslCoreGameData* game_data)
{
    static char line[16384];
    MSL_BINARY_STREAMS();
    for (;;) {
        MslCoreMatch* bases[SERVE_GROUPS_MAX];
        MslCoreInput previous[SERVE_GROUPS_MAX];
        int counts[SERVE_GROUPS_MAX];
        MslCoreMatch* storage;
        MslCoreMatch** matches;
        RunOut* out;
        RunIn* in;
        int groups = 0;
        int group;
        int total = 0;
        int lane;
        int at;
        int status = 0;

        if (fgets(line, sizeof(line), stdin) == NULL ||
            strncmp(line, "quit", 4) == 0)
        {
            return 0;
        }
        if (sscanf(line, "run %d", &groups) != 1 || groups < 1 ||
            groups > SERVE_GROUPS_MAX)
        {
            fprintf(stderr, "serve: expected \"run <groups>\"\n");
            return 2;
        }
        for (group = 0; group < groups; ++group) {
            char* cursor = line;
            char* lanes_text;
            char* tape_path;
            char* state_path;
            char* out_path;
            if (fgets(line, sizeof(line), stdin) == NULL) {
                return 2;
            }
            lanes_text = serve_field(&cursor);
            tape_path = serve_field(&cursor);
            state_path = serve_field(&cursor);
            out_path = serve_field(&cursor);
            counts[group] = lanes_text != NULL ? atoi(lanes_text) : 0;
            if (tape_path == NULL || counts[group] < 1 ||
                total + counts[group] > MAX_LANES)
            {
                fprintf(stderr, "serve: a group is \"<lanes>\\t<tape>\\t<state>\\t<out>\"\n");
                return 2;
            }
            bases[group] = serve_position(game_data, tape_path, state_path,
                                          out_path, &previous[group]);
            if (bases[group] == NULL) {
                return 1;
            }
            total += counts[group];
        }
        storage = (MslCoreMatch*) calloc((size_t) total, sizeof(*storage));
        matches = (MslCoreMatch**) calloc((size_t) total, sizeof(*matches));
        out = (RunOut*) calloc((size_t) total, sizeof(*out));
        in = (RunIn*) calloc((size_t) total, sizeof(*in));
        if (storage == NULL || matches == NULL || out == NULL || in == NULL) {
            fprintf(stderr, "serve: not enough memory for %d lanes\n", total);
            return 1;
        }
        /* each group's first lane is its position itself; the others are
           copies of it */
        at = 0;
        for (group = 0; group < groups; ++group) {
            for (lane = 0; lane < counts[group]; ++lane, ++at) {
                if (lane == 0) {
                    matches[at] = bases[group];
                    continue;
                }
                matches[at] = &storage[at];
                if (msl_core_match_init(matches[at], game_data,
                                        &bases[group]->config,
                                        &previous[group]) != 0 ||
                    msl_core_match_copy(matches[at], bases[group]) != 0)
                {
                    fprintf(stderr, "serve: lane %d could not be made\n", at);
                    return 1;
                }
            }
        }
        for (lane = 0; lane < total; ++lane) {
            fill_run(&out[lane], msl_core_match_output(matches[lane]));
        }
        if (fwrite(out, sizeof(*out), (size_t) total, stdout) !=
                (size_t) total || fflush(stdout) != 0)
        {
            return 1;
        }
        for (;;) {
            static const MslCoreStageEvents no_events = { 0 };
            int c = fgetc(stdin);
            if (c == EOF) {
                status = -1;
                break;
            }
            if (c == 'E') {
                break;
            }
            if (c != 'S' ||
                fread(in, sizeof(*in), (size_t) total, stdin) != (size_t) total)
            {
                fprintf(stderr, "serve: expected a step\n");
                return 2;
            }
            for (lane = 0; lane < total; ++lane) {
                MslCoreInput input;
                TapeFrame carrier;
                memset(&carrier, 0, sizeof(carrier));
                memcpy(carrier.p, in[lane].p, sizeof(carrier.p));
                fill_input(&input, &carrier, matches[lane]->config.num_players);
                if (msl_core_match_step(matches[lane], &input,
                                        matches[lane]->random_seed,
                                        &no_events) != 0)
                {
                    fprintf(stderr, "serve: step failed on lane %d\n", lane);
                    return 1;
                }
                fill_run(&out[lane], msl_core_match_output(matches[lane]));
            }
            if (fwrite(out, sizeof(*out), (size_t) total, stdout) !=
                    (size_t) total || fflush(stdout) != 0)
            {
                return 1;
            }
        }
        for (lane = 0; lane < total; ++lane) {
            msl_core_match_destroy(matches[lane]);
        }
        for (group = 0; group < groups; ++group) {
            free(bases[group]);
        }
        free(storage);
        free(matches);
        free(out);
        free(in);
        if (status < 0) {
            return 0;                   /* the caller is done */
        }
    }
}

/* MSL_RECORDED=<file>: the replay's own positions, frame by frame, to
   resync with (runtime/viewer_track.h, msl_viewer_track_resync). Per frame:
   a uint16 item count, then 4 ports' fighter x, y, 4 followers' x, y (NaN
   for none) and each item's x, y and velocity x, y, all float32. After a
   frame, a position (or an item's velocity) of
   the simulator's within RESYNC_MOST of the recorded one but not equal to it
   (a few float bits: a joint's matrix's last bits) is put back to the
   recorded one, and the row says so; each one is listed in MSL_RESYNC_LOG
   ("<frame index> <kind i|f|n> <slot or port> <field 0 x|1 y> <simulated>
   <recorded>"). A larger difference is left: the game has parted. */
#define RESYNC_MOST 1e-3F

static FILE* recorded;
static FILE* resync_log;

static void resync_one(MslCoreMatch* match, uint32_t index, char kind,
                       int lane, int field, float* simulated, float want)
{
    if (want != want || *simulated == want ||
        fabsf(*simulated - want) > RESYNC_MOST ||
        msl_viewer_track_resync(match, kind, lane, field, want) != 0)
    {
        return;
    }
    if (resync_log != NULL) {
        fprintf(resync_log, "%u %c %d %d %a %a\n", index, kind, lane, field,
                *simulated, want);
    }
    *simulated = want;
}

static void resync_frame(MslCoreMatch* match, uint32_t index, OutFrame* out)
{
    uint16_t items;
    float fighters[16];
    float item_pos[MSL_CORE_MAX_ITEMS * 4];
    int i;
    if (recorded == NULL || fread(&items, sizeof(items), 1, recorded) != 1 ||
        fread(fighters, sizeof(fighters), 1, recorded) != 1 ||
        (items > 0 && fread(item_pos, sizeof(float) * 4,
                            items < MSL_CORE_MAX_ITEMS ? items
                                                       : MSL_CORE_MAX_ITEMS,
                            recorded) == 0))
    {
        recorded = NULL;
        return;
    }
    for (i = 0; i < 4; ++i) {
        resync_one(match, index, 'f', i, 0, &out->pos_x[i], fighters[i * 2]);
        resync_one(match, index, 'f', i, 1, &out->pos_y[i], fighters[i * 2 + 1]);
        resync_one(match, index, 'n', i, 0, &out->follower_pos_x[i],
                   fighters[8 + i * 2]);
        resync_one(match, index, 'n', i, 1, &out->follower_pos_y[i],
                   fighters[8 + i * 2 + 1]);
    }
    if (items != out->item_count) {
        return;
    }
    for (i = 0; i < items && i < MSL_CORE_MAX_ITEMS; ++i) {
        resync_one(match, index, 'i', i, 0, &out->items[i].pos_x,
                   item_pos[i * 4]);
        resync_one(match, index, 'i', i, 1, &out->items[i].pos_y,
                   item_pos[i * 4 + 1]);
        resync_one(match, index, 'i', i, 2, &out->items[i].vel_x,
                   item_pos[i * 4 + 2]);
        resync_one(match, index, 'i', i, 3, &out->items[i].vel_y,
                   item_pos[i * 4 + 3]);
    }
}

int main(int argc, char** argv)
{
    TapeConfig tape;
    MslCoreMatchConfig config;
    MslCoreGameData game_data;
    MslCoreMatch match;
    MslCoreInput previous;
    TapeFrame frame;
    OutFrame out;
    FILE* input;
    FILE* output;
    FILE* track_file = NULL;
    uint32_t index;
    int lanes = 0;
    int arg;
    const char* save_path = NULL;
    const char* load_path = NULL;

    if (argc == 3 && strcmp(argv[2], "--serve") == 0) {
        int status;
        if (msl_core_game_data_init(&game_data, argv[1]) != 0) {
            fprintf(stderr, "game data init failed\n");
            return 1;
        }
        status = serve_mode(&game_data);
        msl_core_game_data_deinit(&game_data);
        return status;
    }
    if (argc == 2 && strcmp(argv[1], "--sizes") == 0) {
        printf("{\"config\": %u, \"frame\": %u, \"out\": %u, "
               "\"run_out\": %u, \"run_in\": %u, \"max_lanes\": %d, "
               "\"save_load\": 1, \"serve\": 1}\n",
               (unsigned) sizeof(TapeConfig), (unsigned) sizeof(TapeFrame),
               (unsigned) sizeof(OutFrame), (unsigned) sizeof(RunOut),
               (unsigned) sizeof(RunIn), (int) MAX_LANES);
        return 0;
    }
    for (arg = 4; arg + 1 < argc; arg += 2) {
        if (strcmp(argv[arg], "--run") == 0) {
            lanes = atoi(argv[arg + 1]);
        } else if (strcmp(argv[arg], "--save") == 0) {
            save_path = argv[arg + 1];
        } else if (strcmp(argv[arg], "--load") == 0) {
            load_path = argv[arg + 1];
#if MSL_GX_RECORD
        } else if (strcmp(argv[arg], "--playback") == 0) {
            /* viewer build: "slippi" (default) or "original", see src/runtime/scalar.c */
            if (msl_playback_set_name(argv[arg + 1]) != 0) {
                fprintf(stderr, "--playback takes slippi or original%c", 10);
                return 2;
            }
#endif
        } else {
            break;
        }
    }
    if (argc < 4 || arg != argc || (load_path != NULL && lanes < 1) ||
        (load_path != NULL && save_path != NULL))
    {
        fprintf(stderr, "usage: %s <data_root> <tape> <out> [--run <lanes>] "
                "[--save <state> | --load <state>]\n", argv[0]);
        return 2;
    }
    input = fopen(argv[2], "rb");
    if (input == NULL || fread(&tape, 1, sizeof(tape), input) != sizeof(tape)) {
        fprintf(stderr, "cannot read tape %s\n", argv[2]);
        return 1;
    }
    if (tape.magic != 0x314C534DU) { /* "MSL1" */
        fprintf(stderr, "bad tape magic %08x\n", tape.magic);
        return 1;
    }
    config_from_tape(&config, &tape);
#if MSL_GX_RECORD
    restore_nametags(argv[2], &tape);
#endif
    if (fread(&frame, 1, sizeof(frame), input) != sizeof(frame)) {
        fprintf(stderr, "tape has no frames\n");
        return 1;
    }
    fill_input(&previous, &frame, tape.num_players);
    if (msl_core_game_data_init(&game_data, argv[1]) != 0) {
        fprintf(stderr, "game data init failed\n");
        return 1;
    }
    // MSL_VIEWER_TRACK=<path>: also write the viewer track there
    // (runtime/viewer_track.h). It only reads simulation state.
    {
        const char* track_path = getenv("MSL_VIEWER_TRACK");
#if MSL_GX_RECORD
        // The viewer build draws the particles and effect models the simulator's recorder hooks feed (msl_viewer_track_on gates
        // them), so the track runs always; written to the null device unless a path is given.
        if (track_path == NULL || track_path[0] == '\0') {
            track_path = "NUL";
        }
#endif
        if (track_path != NULL && track_path[0] != '\0') {
            track_file = fopen(track_path, "w");
            if (track_file == NULL) {
                fprintf(stderr, "cannot write %s\n", track_path);
                return 1;
            }
            msl_viewer_track_begin(track_file);
        }
    }
    if (msl_core_match_storage_init(&match) != 0 ||
        msl_core_match_reset(&match, &game_data, &config, &previous) != 0)
    {
        fprintf(stderr, "match reset failed\n");
        return 1;
    }
    if (load_path != NULL) {
        /* the position saved before (--save), not the tape replayed again */
        size_t size = 0;
        uint8_t* state = read_whole(load_path, &size);
        int status;
        fclose(input);
        if (track_file != NULL) {
            msl_viewer_track_end();
            fclose(track_file);
        }
        if (state == NULL) {
            fprintf(stderr, "cannot read state %s\n", load_path);
            return 3;
        }
        if (msl_core_match_restore(&match, state, size) != 0) {
            free(state);
            fprintf(stderr, "this build cannot take the state %s\n", load_path);
            return 3;
        }
        free(state);
        status = run_mode(&match, &game_data, &previous, lanes);
        msl_core_game_data_deinit(&game_data);
        return status;
    }
    output = fopen(argv[3], "wb");
    if (output == NULL) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    if (getenv("MSL_RECORDED") != NULL && getenv("MSL_RECORDED")[0] != '\0') {
        recorded = fopen(getenv("MSL_RECORDED"), "rb");
        if (getenv("MSL_RESYNC_LOG") != NULL) {
            resync_log = fopen(getenv("MSL_RESYNC_LOG"), "w");
        }
    }
    for (index = 0; index < tape.frame_count; ++index) {
        MslCoreInput step_input;
        MslCoreStageEvents events;
        if (index > 0 &&
            fread(&frame, 1, sizeof(frame), input) != sizeof(frame))
        {
            fprintf(stderr, "short tape at frame %u\n", index);
            return 1;
        }
        fill_input(&step_input, &frame, tape.num_players);
        fill_events(&events, &frame);
        if (msl_core_match_step(&match, &step_input, frame.frame_seed,
                                &events) != 0)
        {
            fprintf(stderr, "step failed at frame %u\n", index);
            return 1;
        }
        fill_output(&out, msl_core_match_output(&match));
        resync_frame(&match, index, &out);
        msl_viewer_track_frame(&match);
#if MSL_GX_RECORD
        msl_gx_frame(&match, (int) index);
#endif
        {
            int phase;
            int kind;
            msl_core_stadium_state(&match, &phase, &kind);
            out.stadium_phase = (uint16_t) phase;
            out.stadium_kind = (uint16_t) kind;
        }
        if (fwrite(&out, 1, sizeof(out), output) != sizeof(out)) {
            fprintf(stderr, "short write at frame %u\n", index);
            return 1;
        }
    }
    fclose(input);
    if (resync_log != NULL) {
        fclose(resync_log);
    }
    if (track_file != NULL) {
        msl_viewer_track_end();
        if (fclose(track_file) != 0) {
            fprintf(stderr, "failed to flush the viewer track\n");
            return 1;
        }
    }
    if (fclose(output) != 0) {
        fprintf(stderr, "failed to flush output\n");
        return 1;
    }
    if (save_path != NULL && save_state(&match, save_path) != 0) {
        fprintf(stderr, "cannot save the state to %s\n", save_path);
        return 1;
    }
    if (lanes > 0) {
        int status = run_mode(&match, &game_data, &previous, lanes);
        msl_core_game_data_deinit(&game_data);
        return status;
    }
    msl_core_match_destroy(&match);
    msl_core_game_data_deinit(&game_data);
    return 0;
}

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
 *   replay_drive <data_root> <tape> <out>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/scalar.h"
#include "runtime/wire.h"

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
    uint8_t pad[2];
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
} OutFrame;

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
    uint32_t index;
    int player;

    if (argc == 2 && strcmp(argv[1], "--sizes") == 0) {
        printf("{\"config\": %u, \"frame\": %u, \"out\": %u}\n",
               (unsigned) sizeof(TapeConfig), (unsigned) sizeof(TapeFrame),
               (unsigned) sizeof(OutFrame));
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: %s <data_root> <tape> <out>\n", argv[0]);
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
    memset(&config, 0, sizeof(config));
    config.stage_id = (uint32_t) tape.stage_id;
    config.frame_id = tape.frame_id;
    config.frame_pre_random_seed = tape.frame_seed;
    config.initial_random_seed = tape.initial_seed;
    config.match_damage_ratio = tape.damage_ratio;
    config.num_players = tape.num_players;
    config.is_teams = tape.is_teams;
    config.friendly_fire = tape.friendly_fire;
    config.stock_count = tape.stock_count;
    config.online_fnmsubs_zero = tape.online_fnmsubs_zero;
    config.brawl_offscreen_damage = tape.brawl_offscreen_damage;
    config.freeze_dead_up_fall_physics = tape.freeze_dead_up_fall_physics;
    config.whispy_dead_fighter_fix = tape.whispy_dead_fighter_fix;
    config.ucf_cardinals_1_0_enabled = tape.ucf_cardinals_1_0_enabled;
    config.ucf_shield_sdi_enabled = tape.ucf_shield_sdi_enabled;
    config.ucf_sdi_enabled = tape.ucf_sdi_enabled;
    config.ucf_shield_drop_extended_enabled =
        tape.ucf_shield_drop_extended_enabled;
    config.ucf_shield_drop_084_enabled = tape.ucf_shield_drop_084_enabled;
    config.stage_event_streams = tape.stage_event_streams;
    for (player = 0; player < 4; ++player) {
        config.players[player].char_id = tape.players[player].char_id;
        config.players[player].team_id = tape.players[player].team_id;
        config.players[player].facing_and_port =
            tape.players[player].facing_and_port;
        config.players[player].costume_id = tape.players[player].costume_id;
        config.players[player].handicap = tape.players[player].handicap;
        config.players[player].start_percent =
            tape.players[player].start_percent;
        config.players[player].cpu_level = tape.players[player].cpu_level;
    }
    if (fread(&frame, 1, sizeof(frame), input) != sizeof(frame)) {
        fprintf(stderr, "tape has no frames\n");
        return 1;
    }
    fill_input(&previous, &frame, tape.num_players);
    if (msl_core_game_data_init(&game_data, argv[1]) != 0) {
        fprintf(stderr, "game data init failed\n");
        return 1;
    }
    if (msl_core_match_storage_init(&match) != 0 ||
        msl_core_match_reset(&match, &game_data, &config, &previous) != 0)
    {
        fprintf(stderr, "match reset failed\n");
        return 1;
    }
    output = fopen(argv[3], "wb");
    if (output == NULL) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
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
        if (fwrite(&out, 1, sizeof(out), output) != sizeof(out)) {
            fprintf(stderr, "short write at frame %u\n", index);
            return 1;
        }
    }
    fclose(input);
    if (fclose(output) != 0) {
        fprintf(stderr, "failed to flush output\n");
        return 1;
    }
    msl_core_match_destroy(&match);
    msl_core_game_data_deinit(&game_data);
    return 0;
}

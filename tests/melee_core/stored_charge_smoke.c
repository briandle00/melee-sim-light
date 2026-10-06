// The stored charges the observation publishes, with extracted data, the real
// scheduler and controller inputs: Giant Punch, Charge Shot, Needle Storm,
// Shadow Ball, Oil Panic, and Kirby's copies of the first four. Every frame
// checks the observed value against what the move has done so far, that both
// observation builders agree byte for byte, that the value sits in its
// player's slot from either viewpoint, and that Fox has none.
#include "runtime/scalar.h"
#include "runtime/observation.h"
#include "ft/fighter.h"
#include "ft/ftcommon.h"
#include "ft/types.h"
#include "ftCommon/forward.h"
#include "ftCommon/ftCo_Fall.h"
#include "ftDonkey/forward.h"
#include "ftDonkey/types.h"
#include "ftGameWatch/forward.h"
#include "ftKirby/forward.h"
#include "ftKirby/ftkirby.h"
#include "ftKirby/types.h"
#include "ftMewtwo/forward.h"
#include "ftMewtwo/types.h"
#include "ftSamus/forward.h"
#include "ftSamus/types.h"
#include "ftSeak/forward.h"
#include "it/types.h"
#include "it/items/itfoxlaser.h"
#include <dolphin/pad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MSL_TEST_FOX = 1, MSL_TEST_FALCO = 22 };

static MslCoreGameData msl_test_game_data;
static MslCoreMatch msl_test_match;
static const MslCoreInput msl_test_neutral;
static const MslCoreInput msl_test_b = { { { PAD_BUTTON_B } } };
static const MslCoreInput msl_test_a = { { { PAD_BUTTON_A } } };
static const MslCoreInput msl_test_jump = { { { PAD_BUTTON_X } } };
static const MslCoreInput msl_test_shield = {
    { { PAD_TRIGGER_R, 0, 0, 0, 0, 0, 255 } }
};
static const MslCoreInput msl_test_shield_b = {
    { { PAD_TRIGGER_R | PAD_BUTTON_B, 0, 0, 0, 0, 0, 255 } }
};
static const MslCoreInput msl_test_down_b = { { { PAD_BUTTON_B, 0, -80 } } };
static const MslCoreInput msl_test_taunt = { { { PAD_BUTTON_UP } } };
// The value observed for port 0 after the last frame.
static int msl_test_charge;
// Whether port 0 was present in that observation, and the stocks of the next setup.
static int msl_test_present;
static unsigned msl_test_stocks = 4;

#define MSL_TEST_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s (frame %d, charge %d)\n", __func__, __LINE__, \
                #condition, msl_test_match.frame_id, msl_test_charge); \
        return -1; \
    } \
} while (0)

static Fighter* msl_test_fighter(unsigned port)
{
    return msl_test_match.fighters[port]->user_data;
}

static int msl_test_frame(const MslCoreInput* input)
{
    MslCoreObservation direct, compared, other;
    MSL_TEST_CHECK(msl_core_match_step(&msl_test_match, input, msl_test_match.random_seed,
                                       &(MslCoreStageEvents) { 0 }) == 0);
    MSL_TEST_CHECK(msl_core_match_write_observation(&msl_test_match, 0, &direct) == 0);
    MSL_TEST_CHECK(msl_core_match_write_observation_from_compare(&msl_test_match, 0, &compared) == 0);
    MSL_TEST_CHECK(msl_core_match_write_observation(&msl_test_match, 1, &other) == 0);
    msl_test_charge = direct.stored_charge[0];
    msl_test_present = direct.slots[0].present;
    MSL_TEST_CHECK(memcmp(&direct, &compared, sizeof(direct)) == 0);
    // Singles: each viewpoint has itself in slot 0 and the opponent in slot 1.
    MSL_TEST_CHECK(other.slots[1].source_player == 0 &&
                   other.stored_charge[1] == direct.stored_charge[0]);
    MSL_TEST_CHECK(direct.stored_charge[1] == 0 && other.stored_charge[0] == 0);
    MSL_TEST_CHECK(direct.stored_charge[2] == 0 && direct.stored_charge[3] == 0);
    return 0;
}

static int msl_test_setup(unsigned character, unsigned opponent)
{
    MslCoreMatchConfig config = { 0 };
    config.stage_id = 32;
    config.frame_id = -123;
    config.initial_random_seed = config.frame_pre_random_seed = 1;
    config.match_damage_ratio = 1;
    config.num_players = 2;
    config.stock_count = msl_test_stocks;
    config.players[0].char_id = character;
    config.players[1].char_id = opponent;
    MSL_TEST_CHECK((msl_test_match.memory.arena == NULL
               ? msl_core_match_init(&msl_test_match, &msl_test_game_data, &config, &msl_test_neutral)
               : msl_core_match_reset(&msl_test_match, &msl_test_game_data, &config, &msl_test_neutral)) == 0);
    for (unsigned i = 0; i < 150; ++i) {
        MSL_TEST_CHECK(msl_test_frame(&msl_test_neutral) == 0 && msl_test_charge == 0);
    }
    return 0;
}

// `frames` frames of `input`, the value staying at `charge` on every one.
static int msl_test_hold(const MslCoreInput* input, unsigned frames, int charge)
{
    for (unsigned i = 0; i < frames; ++i) {
        MSL_TEST_CHECK(msl_test_frame(input) == 0 && msl_test_charge == charge);
    }
    return 0;
}

// Holds `input` until the value is `target`: it may only stay or go up by one.
static int msl_test_rise(const MslCoreInput* input, int target, unsigned limit)
{
    for (unsigned i = 0; msl_test_charge != target; ++i) {
        int before = msl_test_charge;
        MSL_TEST_CHECK(i < limit);
        MSL_TEST_CHECK(msl_test_frame(input) == 0);
        MSL_TEST_CHECK(msl_test_charge == before || msl_test_charge == before + 1);
    }
    return 0;
}

// Holds `input` until port 0 is in `motion`, the value staying at `charge`.
static int msl_test_until(const MslCoreInput* input, int motion, unsigned limit, int charge)
{
    for (unsigned i = 0; (int) msl_test_fighter(0)->motion_id != motion; ++i) {
        MSL_TEST_CHECK(i < limit);
        MSL_TEST_CHECK(msl_test_frame(input) == 0 && msl_test_charge == charge);
    }
    return 0;
}

// A short hop and a jab from standing: the value is kept through both.
static int msl_test_other_actions(int charge)
{
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, charge) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_jump, 2, charge) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 10, charge) == 0);
    MSL_TEST_CHECK(msl_test_fighter(0)->ground_or_air == GA_Air);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, charge) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_a, 1, charge) == 0);
    MSL_TEST_CHECK(msl_test_fighter(0)->motion_id ==
                   (msl_test_fighter(0)->kind == FTKIND_GAMEWATCH ? ftGw_MS_Attack11
                                                                 : ftCo_MS_Attack11));
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, charge) == 0);
    return 0;
}

// Past the left blast zone: the stock goes at once, the value stays `dead`
// through the death animation and is `reborn` from the first frame of
// Rebirth, where the game runs the fighter's OnDeath.
static int msl_test_ko(int dead, int reborn)
{
    Fighter* fp = msl_test_fighter(0);
    int stocks = msl_test_match.source.player.slots[fp->player_id].stocks;
    fp->cur_pos = (Vec3) { -300, 40, 0 };
    fp->prev_pos = fp->cur_pos;
    ftCommon_8007D5D4(fp);
    ftCo_Fall_Enter(msl_test_match.fighters[0]);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 1, dead) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftCo_MS_DeadLeft &&
                   msl_test_match.source.player.slots[fp->player_id].stocks == stocks - 1);
    for (unsigned i = 0; fp->motion_id != ftCo_MS_Rebirth; ++i) {
        MSL_TEST_CHECK(i < 120);
        MSL_TEST_CHECK(msl_test_frame(&msl_test_neutral) == 0);
        MSL_TEST_CHECK(msl_test_charge == (fp->motion_id == ftCo_MS_DeadLeft ? dead : reborn));
    }
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_RebirthWait, 300, reborn) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 600, reborn) == 0);
    return 0;
}

// Giant Punch counts arm swings. Three swings and a shield out of the wind-up
// keep 3; winding up again goes on from 3 to the full count, where the game
// ends the wind-up itself; the punch takes the count. A KO clears it.
static int msl_test_giant_punch(void)
{
    Fighter* fp;
    int full;
    MSL_TEST_CHECK(msl_test_setup(FTKIND_DONKEY, MSL_TEST_FOX) == 0);
    fp = msl_test_fighter(0);
    full = ((ftDonkeyAttributes*) fp->dat_attrs)->SpecialN.x2C_MAX_ARM_SWINGS;
    MSL_TEST_CHECK(full > 4 && full <= UINT8_MAX);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftDk_MS_SpecialNStart);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 2, 200) == 0);
    // The shield is taken at the top of the next swing, which still counts.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 2) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 3, 60) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftDk_MS_SpecialNCancel);
    MSL_TEST_CHECK(msl_test_other_actions(3) == 0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 3) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftDk_MS_SpecialNStart);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, full, 600) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftCo_MS_Wait);
    MSL_TEST_CHECK(msl_test_other_actions(full) == 0);

    // The punch: the count moves into the move on the frame it starts.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftDk_MS_SpecialNFull && fp->mv.dk.specialn.xC == full);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);

    // One swing, punched from the wind-up.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 1, 200) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftDk_MS_SpecialN && fp->mv.dk.specialn.xC == 1);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);

    // Two swings stored, then a KO.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 1, 200) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 1) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 2, 60) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, 2) == 0);
    MSL_TEST_CHECK(msl_test_ko(2, 0) == 0);
    fprintf(stderr, "giant punch: full at %d swings\n", full);
    return 0;
}

// A laser from the right, followed with `input` held until it hurts port 0.
// Returns the value on the frame the damage lands; before it the value must
// be `charge` or one more (a charge in progress).
static int msl_test_laser(const MslCoreInput* input, int charge)
{
    Fighter* fp = msl_test_fighter(0);
    float percent = fp->dmg.x1830_percent;
    Vec3 pos = { 30, 8, 0 };
    it_8029C6A4(3.14159265358979323846F, 4, msl_test_match.fighters[1], &pos,
                It_Kind_Falco_Laser);
    for (unsigned i = 0; i < 60; ++i) {
        MSL_TEST_CHECK(msl_test_frame(input) == 0);
        if (fp->dmg.x1830_percent > percent) {
            return msl_test_charge;
        }
        MSL_TEST_CHECK(msl_test_charge == charge || msl_test_charge == charge + 1);
    }
    MSL_TEST_CHECK(!"the laser never landed");
    return -1;
}

static int msl_test_setup_in_range(unsigned character)
{
    Fighter* fp;
    MSL_TEST_CHECK(msl_test_setup(character, MSL_TEST_FALCO) == 0);
    fp = msl_test_fighter(0);
    fp->cur_pos = (Vec3) { -20, 0, 0 };
    fp->prev_pos = fp->cur_pos;
    fp->facing_dir = 1;
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 1, 0) == 0);
    return 0;
}

// A hit leaves a stored count alone, and takes it when it lands during the
// wind-up (the move's own damage callback clears it).
static int msl_test_hit_during_wind_up(void)
{
    Fighter* fp;
    MSL_TEST_CHECK(msl_test_setup_in_range(FTKIND_DONKEY) == 0);
    fp = msl_test_fighter(0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 1, 200) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 1) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 2, 60) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, 2) == 0);
    MSL_TEST_CHECK(msl_test_laser(&msl_test_neutral, 2) == 2);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 2) == 0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 2) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 3, 200) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftDk_MS_SpecialNLoop);
    MSL_TEST_CHECK(msl_test_laser(&msl_test_neutral, 3) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);
    return 0;
}

// The same for Charge Shot, Needle Storm and Shadow Ball: a hit during the
// charge takes the count. Mewtwo's damage callback spares a full one.
static int msl_test_hit_during_charge(FighterKind kind)
{
    const MslCoreInput* charge = kind == FTKIND_SEAK ? &msl_test_b : &msl_test_neutral;
    MSL_TEST_CHECK(msl_test_setup_in_range(kind) == 0);
    MSL_TEST_CHECK(msl_test_frame(&msl_test_b) == 0);
    MSL_TEST_CHECK(msl_test_rise(charge, 2, 300) == 0);
    MSL_TEST_CHECK(msl_test_laser(charge, 2) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);
    if (kind == FTKIND_MEWTWO) {
        Fighter* fp = msl_test_fighter(0);
        int full = (int) ((ftMewtwoAttributes*) fp->dat_attrs)->x0_MEWTWO_SHADOWBALL_CHARGE_CYCLES;
        MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
        MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, full, 900) == 0);
        MSL_TEST_CHECK(fp->motion_id == ftMt_MS_SpecialNLoopFull);
        MSL_TEST_CHECK(msl_test_laser(&msl_test_neutral, full) == full);
        MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, full) == 0);
    }
    return 0;
}

// Charge Shot counts charge steps. A shield out of the charge keeps the
// count, charging again goes on from it to the full count, where the game
// ends the charge itself, and the shot takes it.
static int msl_test_charge_shot(void)
{
    Fighter* fp;
    int full;
    MSL_TEST_CHECK(msl_test_setup(FTKIND_SAMUS, MSL_TEST_FOX) == 0);
    fp = msl_test_fighter(0);
    full = (int) ((ftSs_DatAttrs*) fp->dat_attrs)->x18;
    MSL_TEST_CHECK(full > 4 && full <= UINT8_MAX);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSs_MS_SpecialNStart);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 3, 200) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSs_MS_SpecialNHold);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 3) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSs_MS_SpecialNCancel);
    MSL_TEST_CHECK(msl_test_other_actions(3) == 0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 3) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, full, 600) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSs_MS_SpecialNCancel);
    MSL_TEST_CHECK(msl_test_other_actions(full) == 0);

    // The shot: full until it leaves the cannon, none from that frame.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, full) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftSs_MS_SpecialN, 60, full) == 0);
    for (unsigned i = 0; msl_test_charge != 0; ++i) {
        MSL_TEST_CHECK(i < 60);
        MSL_TEST_CHECK(msl_test_frame(&msl_test_neutral) == 0);
        MSL_TEST_CHECK(msl_test_charge == full || msl_test_charge == 0);
        MSL_TEST_CHECK(fp->motion_id == ftSs_MS_SpecialN);
    }
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);

    // Two steps stored, then a KO.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 2, 200) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 2) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, 2) == 0);
    MSL_TEST_CHECK(msl_test_ko(2, 0) == 0);
    fprintf(stderr, "charge shot: full at %d steps\n", full);
    return 0;
}

// Needle Storm counts needles in hand, 1 from the start of the move and at
// most 6. A shield out of the charge keeps them; the throw takes them one at
// a time.
static int msl_test_needles(void)
{
    Fighter* fp;
    MSL_TEST_CHECK(msl_test_setup(FTKIND_SEAK, MSL_TEST_FOX) == 0);
    fp = msl_test_fighter(0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 1) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSk_MS_SpecialNStart);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_b, 4, 300) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield_b, 1, 4) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSk_MS_SpecialNCancel);
    MSL_TEST_CHECK(msl_test_other_actions(4) == 0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 4) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_b, 6, 300) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 120, 6) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSk_MS_SpecialNLoop);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield_b, 1, 6) == 0);
    MSL_TEST_CHECK(msl_test_other_actions(6) == 0);

    // The throw: release B in the charge loop.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 6) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_b, ftSk_MS_SpecialNLoop, 60, 6) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 1, 6) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftSk_MS_SpecialNEnd);
    for (unsigned i = 0; msl_test_charge != 0; ++i) {
        int before = msl_test_charge;
        MSL_TEST_CHECK(i < 120);
        MSL_TEST_CHECK(msl_test_frame(&msl_test_neutral) == 0);
        MSL_TEST_CHECK(msl_test_charge == before || msl_test_charge == before - 1);
        MSL_TEST_CHECK(fp->motion_id == ftSk_MS_SpecialNEnd);
    }
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);

    // Two needles stored, then a KO.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 1) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_b, 2, 300) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield_b, 1, 2) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, 2) == 0);
    MSL_TEST_CHECK(msl_test_ko(2, 0) == 0);
    return 0;
}

// Shadow Ball counts charge cycles: kept through a shield out of the charge,
// continued to the full count, taken by the throw.
static int msl_test_shadow_ball(void)
{
    Fighter* fp;
    int full;
    MSL_TEST_CHECK(msl_test_setup(FTKIND_MEWTWO, MSL_TEST_FOX) == 0);
    fp = msl_test_fighter(0);
    full = (int) ((ftMewtwoAttributes*) fp->dat_attrs)->x0_MEWTWO_SHADOWBALL_CHARGE_CYCLES;
    MSL_TEST_CHECK(full > 4 && full <= UINT8_MAX);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftMt_MS_SpecialNStart);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 3, 300) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftMt_MS_SpecialNLoop);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 3) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftMt_MS_SpecialNCancel);
    MSL_TEST_CHECK(msl_test_other_actions(3) == 0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 3) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, full, 900) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftMt_MS_SpecialNLoopFull);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 30, full) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, full) == 0);
    MSL_TEST_CHECK(msl_test_other_actions(full) == 0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, full) == 0);
    for (unsigned i = 0; msl_test_charge != 0; ++i) {
        MSL_TEST_CHECK(i < 120);
        MSL_TEST_CHECK(msl_test_frame(&msl_test_neutral) == 0);
        MSL_TEST_CHECK(msl_test_charge == full || msl_test_charge == 0);
    }
    MSL_TEST_CHECK(fp->motion_id == ftMt_MS_SpecialNEnd);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);

    // Two cycles stored, then a KO: Mewtwo's own death callback, left by the
    // move, takes them on the frame of the KO.
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 2, 300) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 2) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, 2) == 0);
    MSL_TEST_CHECK(msl_test_ko(0, 0) == 0);
    fprintf(stderr, "shadow ball: full at %d cycles\n", full);
    return 0;
}

// Oil Panic counts caught shots, full at 3. The count is kept through a KO
// (ftGw_Init_OnDeath clears the stored damage, not the count), and the spill
// takes it.
static int msl_test_oil_panic(void)
{
    Fighter* fp;
    MSL_TEST_CHECK(msl_test_setup(FTKIND_GAMEWATCH, MSL_TEST_FALCO) == 0);
    fp = msl_test_fighter(0);
    fp->cur_pos = (Vec3) { -20, 0, 0 };
    fp->prev_pos = fp->cur_pos;
    fp->facing_dir = 1;
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_down_b, 20, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftGw_MS_SpecialLw);
    for (int shot = 1; shot <= 3; ++shot) {
        Vec3 pos = { 30, 8, 0 };
        it_8029C6A4(3.14159265358979323846F, 4, msl_test_match.fighters[1], &pos,
                    It_Kind_Falco_Laser);
        MSL_TEST_CHECK(msl_test_rise(&msl_test_down_b, shot, 60) == 0);
        MSL_TEST_CHECK(msl_test_hold(&msl_test_down_b, 50, shot) == 0);
    }
    MSL_TEST_CHECK(msl_test_other_actions(3) == 0);
    MSL_TEST_CHECK(msl_test_ko(3, 3) == 0);
    MSL_TEST_CHECK(fp->fv.gw.x223C_panicDamage == 0);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_down_b, 1, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftGw_MS_SpecialLwShoot);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);
    return 0;
}

// A player without stocks is absent from the observation, and so is the
// count the game still keeps for him.
static int msl_test_absent_after_last_stock(void)
{
    Fighter* fp;
    Vec3 pos = { 30, 8, 0 };
    int absent = 0;
    msl_test_stocks = 1;
    MSL_TEST_CHECK(msl_test_setup_in_range(FTKIND_GAMEWATCH) == 0);
    msl_test_stocks = 4;
    fp = msl_test_fighter(0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_down_b, 20, 0) == 0);
    it_8029C6A4(3.14159265358979323846F, 4, msl_test_match.fighters[1], &pos,
                It_Kind_Falco_Laser);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_down_b, 1, 60) == 0);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 120, 1) == 0);
    fp->cur_pos = (Vec3) { -300, 40, 0 };
    fp->prev_pos = fp->cur_pos;
    ftCommon_8007D5D4(fp);
    ftCo_Fall_Enter(msl_test_match.fighters[0]);
    for (unsigned i = 0; i < 300; ++i) {
        MSL_TEST_CHECK(msl_test_frame(&msl_test_neutral) == 0);
        MSL_TEST_CHECK(msl_test_charge == (msl_test_present ? 1 : 0));
        absent += !msl_test_present;
    }
    MSL_TEST_CHECK(absent > 100 && !msl_test_present && fp->fv.gw.x2238_panicCharge == 1);
    return 0;
}

// Kirby inhales and swallows Donkey Kong, then winds up the copied punch: the
// observed value is the copy's own count, and goes with the hat when a taunt
// throws it away.
static int msl_test_kirby_swallows_donkey_kong(void)
{
    Fighter* fp;
    Fighter* opponent;
    int full;
    MSL_TEST_CHECK(msl_test_setup(FTKIND_KIRBY, FTKIND_DONKEY) == 0);
    fp = msl_test_fighter(0);
    opponent = msl_test_fighter(1);
    fp->cur_pos = (Vec3) { 0, 0, 0 };
    fp->prev_pos = fp->cur_pos;
    fp->facing_dir = 1;
    opponent->cur_pos = (Vec3) { 10, 0, 0 };
    opponent->prev_pos = opponent->cur_pos;
    opponent->facing_dir = -1;
    full = ((ftKb_DatAttrs*) fp->dat_attrs)->specialn_dk_swings_to_full_charge;
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    for (unsigned i = 0; fp->fv.kb.hat.kind != FTKIND_DONKEY; ++i) {
        // B again, on a press edge inside the wait state, swallows.
        MSL_TEST_CHECK(i < 200);
        MSL_TEST_CHECK(msl_test_hold(fp->motion_id == ftKb_MS_EatWait && (i % 4) < 2
                                         ? &msl_test_b : &msl_test_neutral, 1, 0) == 0);
    }
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);
    opponent->cur_pos = (Vec3) { 60, 0, 0 };
    opponent->prev_pos = opponent->cur_pos;

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 0) == 0);
    MSL_TEST_CHECK(fp->motion_id == ftKb_MS_DkSpecialNStart);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 2, 200) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_shield, 1, 2) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, 3, 60) == 0);
    MSL_TEST_CHECK(msl_test_other_actions(3) == 0);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 3) == 0);
    MSL_TEST_CHECK(msl_test_rise(&msl_test_neutral, full, 600) == 0);
    MSL_TEST_CHECK(msl_test_other_actions(full) == 0);

    // The taunt throws the hat away on its first frame.
    MSL_TEST_CHECK(fp->fv.kb.hat.kind == FTKIND_DONKEY && fp->fv.kb.xBC == full);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_taunt, 1, 0) == 0);
    MSL_TEST_CHECK(fp->fv.kb.hat.kind == FTKIND_KIRBY && fp->motion_id != ftCo_MS_Wait);
    MSL_TEST_CHECK(msl_test_until(&msl_test_neutral, ftCo_MS_Wait, 200, 0) == 0);
    fprintf(stderr, "kirby: copied giant punch full at %d swings\n", full);
    return 0;
}

// The other three copies, hat given directly: the observed value is the count
// of the hat Kirby wears, kept through a shield out of the charge, continued
// to the copy's full count, and gone at a KO.
static int msl_test_kirby_copy(FighterKind kind)
{
    const MslCoreInput* charge = kind == FTKIND_SEAK ? &msl_test_b : &msl_test_neutral;
    const MslCoreInput* shield = kind == FTKIND_SEAK ? &msl_test_shield_b : &msl_test_shield;
    Fighter* fp;
    ftKb_DatAttrs* da;
    int full;
    MSL_TEST_CHECK(msl_test_setup(FTKIND_KIRBY, MSL_TEST_FOX) == 0);
    fp = msl_test_fighter(0);
    da = fp->dat_attrs;
    full = kind == FTKIND_SAMUS ? (int) da->specialn_ss_charge_time :
           kind == FTKIND_MEWTWO ? (int) da->specialn_mt_charge_time : 6;
    ftKb_SpecialN_800F1BAC(msl_test_match.fighters[0], kind, false);
    MSL_TEST_CHECK(msl_test_hold(&msl_test_neutral, 1, 0) == 0);
    MSL_TEST_CHECK(msl_test_frame(&msl_test_b) == 0);
    MSL_TEST_CHECK(msl_test_rise(charge, 3, 300) == 0);
    MSL_TEST_CHECK(msl_test_hold(shield, 1, 3) == 0);
    MSL_TEST_CHECK(msl_test_other_actions(3) == 0);
    MSL_TEST_CHECK((kind == FTKIND_SAMUS ? fp->fv.kb.xA8 :
                    kind == FTKIND_MEWTWO ? fp->fv.kb.x9C : fp->fv.kb.xB4) == 3);

    MSL_TEST_CHECK(msl_test_hold(&msl_test_b, 1, 3) == 0);
    MSL_TEST_CHECK(msl_test_rise(charge, full, 900) == 0);
    MSL_TEST_CHECK(msl_test_hold(charge, 30, full) == 0);
    MSL_TEST_CHECK(msl_test_hold(shield, 1, full) == 0);
    MSL_TEST_CHECK(msl_test_other_actions(full) == 0);
    // Kirby's death callback takes the copy's count on the frame of the KO;
    // the hat is gone by the respawn.
    MSL_TEST_CHECK(msl_test_ko(0, 0) == 0);
    MSL_TEST_CHECK(fp->fv.kb.hat.kind == FTKIND_KIRBY);
    fprintf(stderr, "kirby: copy of fighter %d full at %d\n", (int) kind, full);
    return 0;
}

int main(int argc, char** argv)
{
    int result;
    if (argc != 2 || msl_core_game_data_init(&msl_test_game_data, argv[1])) return 1;
    result = msl_test_giant_punch() || msl_test_hit_during_wind_up() ||
             msl_test_hit_during_charge(FTKIND_SAMUS) ||
             msl_test_hit_during_charge(FTKIND_SEAK) ||
             msl_test_hit_during_charge(FTKIND_MEWTWO) ||
             msl_test_charge_shot() || msl_test_needles() ||
             msl_test_shadow_ball() || msl_test_oil_panic() ||
             msl_test_absent_after_last_stock() ||
             msl_test_kirby_swallows_donkey_kong() || msl_test_kirby_copy(FTKIND_SAMUS) ||
             msl_test_kirby_copy(FTKIND_MEWTWO) || msl_test_kirby_copy(FTKIND_SEAK);
    msl_core_match_destroy(&msl_test_match);
    msl_core_game_data_deinit(&msl_test_game_data);
    return result != 0;
}

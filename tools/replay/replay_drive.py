"""Replay a .slp through the core and diff it against the recording.

Builds the tape `replay_drive` consumes - match config, per-frame recorded
inputs, the frame-start RNG, the fighter RNG and the stage events - runs it,
and compares every frame against the file.

  python -m tools.replay.replay_drive <replay.slp> [...] [--json out.json]
  python -m tools.replay.replay_drive --list files.txt --profile ucf084

**This is not validation and its answers are not verdicts.** The strict gate
lives in tools/validation; admission is what decides whether a recording may
be compared at all, and agent_docs/VALIDATION.md forbids exactly what
`state_matches_recording` does when the recording's own profile does not
reproduce it: it tries each other UCF capability profile and keeps the one
that reproduces the file, where the gate requires reviewed provenance and
says "never pick a profile solely because it makes a recording pass".
Nothing here may be used to admit a capture, record a suite result, write an
output lock, or claim that the simulator is or is not exact. It answers one
question for one consumer: may I start something from the position this
recording reached at this frame?

Four things are per-recording and have to be right or nothing matches:

  * the internal character id, from the post-frame rows - the external id in
    the game-start block picks another character's entry data;
  * the UCF capability profile: first the one the recording's gecko list
    names (UCF 0.8 or 0.84 shield drop, and which of the later injections
    it carries), then the fitted ones;
  * on Pokemon Stadium, which patch set owns the transformations (frozen,
    Slippi's preloaded ones, or Slippi Online's synchronous load), also from
    the gecko list. A recording with neither patch set streamed the stage's
    archives from the disc, and is refused;
  * where the fighters enter: Slippi's NeutralSpawn when the gecko list
    carries it, otherwise each port's own stage spawn point (console setups).

Recordings from before Slippi 3.17 have no raw C-stick bytes. One processed
reading cannot be turned back into raw bytes without a choice (see
reconstructed_cstick), and state_matches_recording makes that choice per
held stick position against the recording (drive_fitted) - fitting, with the
same standing as the profile fit above.

"Matches" means every frame's fighters (both ports' leaders on seven lanes,
and Nana on the same seven wherever the recording has her) and every item on
its gameplay lanes. On Slippi 3.18+ the Stadium transformation events are
there to compare too (stadium_diff), but they are a diagnostic: a wrong
transformation shows up in the fighters soon enough.

Rollback re-simulations write a second row for the same frame id; the last row
for an id is the finalized one, as in native.c::build_finalized_rows.
"""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from peppi_py import _read_slippi  # noqa: E402

DRIVER = Path(os.environ.get(
    "MSL_REPLAY_DRIVE",
    ROOT / "build/melee_core/python/replay_drive.exe"))
DATA_ROOT = Path(os.environ.get("MSL_DATA_DIR", ROOT / "data")) / "raw"

CONFIG_FMT = "<IiiIIf17B32BI"
FRAME_FMT = "<II4B2f" + "H4b3B4f" * 4
MAX_ITEMS = 15
# replay_drive.c OutItem: exists, state, owner, misc_mask, type, instance_id,
# misc[4], damage, pad, spawn_id, direction, vel_x, vel_y, pos_x, pos_y, timer.
ITEM_FMT = "BBbBHH4BHHI6f"
ITEM_FIELDS = 17
OUT_FMT = ("<iI16f4H4h4H4H32B"
           # follower present/facing/stocks, item count and pad, Stadium
           # phase and kind, follower action, follower pos x/y, percent,
           # shield
           "12BB3x2H4H16f" + ITEM_FMT * MAX_ITEMS)
CONFIG_SIZE = struct.calcsize(CONFIG_FMT)
FRAME_SIZE = struct.calcsize(FRAME_FMT)
OUT_SIZE = struct.calcsize(OUT_FMT)
TAPE_MAGIC = 0x314C534D  # "MSL1"

LEGAL_STAGES = (2, 3, 8, 28, 31, 32)

NOT_A_VERDICT = ('not admission and not a validation result: the inputs '
                 'profile is fitted by trying each one, which the strict '
                 'gate forbids (agent_docs/VALIDATION.md)')

# cardinals, shield SDI, SDI, extended shield drop, 0.84 shield drop, and
# the UCF injections absent (1 tumble, 2 dashback, 4 shield drop: wire.h
# MSL_PATCH_UCF_*_ABSENT, slippi_patches bits 4 to 6).
UCF_PROFILES = {
    "ucf10": (1, 1, 1, 1, 1, 0),
    "ucf084": (0, 0, 0, 1, 1, 0),
    "ucf08": (0, 0, 0, 0, 0, 0),
    "ucf074": (0, 0, 0, 0, 0, 1),
    "vanilla": (0, 0, 0, 0, 0, 7),
}
PROFILE_ORDER = ("ucf10", "ucf084", "ucf08", "ucf074", "vanilla")
UCF_TUMBLE = 0xC20908F4
UCF_DASHBACK = 0xC20C9A44
# The absent mask's place in slippi_patches (wire.h MSL_PATCH_UCF_TUMBLE_ABSENT).
UCF_ABSENT_SHIFT = 4

# The UCF injections a recording's gecko list carries, by address, in
# UCF_PROFILES order. UCF 0.8 and 0.84 both inject the classic shield drop at
# 0x800998A4; 0.84's is the 208-byte block (External/UCF 0.84/UCF/
# UCF Shield Drop.asm), 0.8's the 312-byte one (External/UCF 0.8/Logic/
# UCF SD.asm), which has no platform check.
UCF_CODES = (0xC206B460,   # UCF Pad Buffer + 1.0 Cardinals
             0xC2093294,   # UCF Shield SDI
             0xC208E54C,   # UCF SDI
             0xC209A0B8)   # UCF Shield Drop Extended
UCF_SHIELD_DROP = 0xC20998A4
UCF_084_SHIELD_DROP_SIZE = 208


def recorded_profile(raw):
    """The UCF profile the recording's own gecko list says it ran, as a
    UCF_PROFILES-style name ("ucf:", five bits and the absent-code mask), or
    None when the recording has no gecko list to go by."""
    from tools.validation.admission import gecko_codes
    try:
        codes = gecko_codes(raw)
    except ValueError:
        return None
    if not codes:
        return None
    drop = codes.get(UCF_SHIELD_DROP)
    bits = [1 if address in codes else 0 for address in UCF_CODES]
    bits.append(1 if drop is not None and
                len(drop) == UCF_084_SHIELD_DROP_SIZE else 0)
    absent = ((0 if UCF_TUMBLE in codes else 1) |
              (0 if UCF_DASHBACK in codes else 2) |
              (0 if drop is not None else 4))
    return "ucf:" + "".join(str(bit) for bit in bits) + str(absent)


def profile_bits(profile):
    if profile in UCF_PROFILES:
        return UCF_PROFILES[profile]
    if isinstance(profile, str) and profile.startswith("ucf:"):
        bits = tuple(int(bit) for bit in profile[4:])
        return bits if len(bits) == 6 else bits + (0,)
    raise KeyError(profile)


def candidate_profiles(replay):
    """The recording's own profile first, then the fitted ones it is not."""
    out = []
    seen = set()
    for profile in ((replay.get("ucf_profile"),) + PROFILE_ORDER):
        if profile is None or profile_bits(profile) in seen:
            continue
        seen.add(profile_bits(profile))
        out.append(profile)
    return tuple(out)

FIELDS = ("pos_x", "pos_y", "percent", "action_id", "stocks", "facing",
          "shield_hp")


def available() -> bool:
    return DRIVER.is_file() and (DATA_ROOT / "manifest.json").is_file()


def column(node, name):
    return node.field(name).to_numpy(zero_copy_only=False)


def trigger_u8(values):
    """native.c::trigger_u8 - lrintf, half away from zero, clamped at 140."""
    out = np.asarray(values, dtype=np.float64)
    out = np.where(out > 0.0, np.floor(out * 140.0 + 0.5), 0.0)
    return np.clip(out, 0, 140).astype(np.uint8)


# replay_drive.c TapeConfig.stadium_transformations, wire.h MSL_STADIUM_*.
STADIUM_FROZEN, STADIUM_PRELOADED, STADIUM_ONLINE_LOADED = 0, 1, 2


def stadium_transformations(start, raw):
    """How this recording's Pokemon Stadium transforms: a STADIUM_* value,
    or None when it does and no deterministic patch set owns the read.

    Frozen is the console toggle's is_frozen_ps, the online toggle at
    0x801D457C (IngameCheckIfFrozen: its static byte is the toggle), or the
    legacy online bypass at 0x801D4578. Unfrozen needs one of the two Slippi
    patch sets that make the transformation read deterministic; a recording
    with neither streamed the archive from the disc asynchronously, and
    nothing in the file says when that read finished."""
    if int(start["stage"]) != 3:
        return STADIUM_FROZEN
    from tools.validation.admission import gecko_codes, stadium_transformations
    return stadium_transformations(start, gecko_codes(raw))


def finalized_rows(ids):
    """The row index to read for each frame id, in frame order, and how many
    trailing frames were dropped as belonging to a discarded timeline.

    Rollback re-simulations write a second row for the same frame id; the
    last row for an id is the finalized one. A frame whose last row was
    written before an earlier frame's rewrite belongs to the discarded
    timeline: the game ended before the re-simulation reached it. Online
    games can end that way, and those rows are no one's frame to match."""
    ids = np.asarray(ids)
    order = np.argsort(ids, kind="stable")
    keep = [order[i] for i in range(len(order))
            if i + 1 == len(order) or ids[order[i + 1]] != ids[order[i]]]
    keep = np.asarray(keep, dtype=np.int64)
    if not len(keep):
        return keep, 0
    stale = np.nonzero(keep < np.maximum.accumulate(keep))[0]
    if len(stale):
        return keep[:int(stale[0])], len(keep) - int(stale[0])
    return keep, 0


def raw_cpu_levels(raw):
    """Each port's CPU level byte from the raw Game Start event (Slippi spec
    offset 0x74 + 0x24 * port), or None when the file cannot be walked.

    The byte is there for every port, humans included, and Slippi playback
    restores it. It matters for a human's Nana: her AI is set up with the
    player's level (ftCo_800B9704: x34 = (10 - level) * (15 r + 15) + 10),
    and peppi reports the level only for CPU players."""
    import struct
    try:
        at = raw.index(b"raw[$U#l") + 8
        size = struct.unpack(">I", raw[at:at + 4])[0]
        body = raw[at + 4:at + 4 + size]
        if body[0] != 0x35:
            return None
        sizes = {}
        for k in range(2, 1 + body[1], 3):
            sizes[body[k]] = struct.unpack(">H", body[k + 1:k + 3])[0]
        start = 1 + body[1]
        if body[start] != 0x36 or sizes.get(0x36, 0) < 0x74 + 0x24 * 3:
            return None
        return [body[start + 0x74 + 0x24 * port] for port in range(4)]
    except (ValueError, IndexError, struct.error):
        return None


HUMAN_LEVEL = 0x80      # wire.h MSL_CORE_HUMAN_LEVEL


def tape_level(player):
    """MslCoreMatchPlayerConfig.cpu_level for a recorded player: the CPU
    level for a CPU, or MSL_CORE_HUMAN_LEVEL | level for a human whose
    level byte is set."""
    if player.get("type") != "Human":
        return player["cpu_level"]
    level = player.get("human_level", 0)
    return HUMAN_LEVEL | level if 1 <= level <= 9 else 0


def read(path):
    """Everything the tape needs plus the per-frame truth to compare against."""
    game = _read_slippi(str(path), False)
    with open(path, "rb") as handle:
        levels = raw_cpu_levels(handle.read())
    start = game.start
    frames = game.frames
    names = frames.type.names
    ids = column(frames, "id")
    # Slippi 2.2+ records the RNG at the start of each frame. Before that the
    # first fighter's pre-frame row is the earliest observation: the seed
    # there is the frame's own unless a stage or item callback drew before
    # the fighters, which the tape then cannot see.
    frame_seed = (column(frames.field("start"), "random_seed")
                  if "start" in names else None)
    ports = frames.field("ports" if "ports" in names else "port")
    players = []
    for index, player in enumerate(start["players"]):
        leader = ports.field(index).field("leader")
        pre, post = leader.field("pre"), leader.field("post")
        pre_names = set(pre.type.names)
        players.append({
            "port": "P1P2P3P4".index(str(player["port"])) // 2,
            "character": int(player["character"]),
            "type": str(player["type"]),
            "costume": int(player["costume"]),
            "handicap": int(player["handicap"] or 9),
            "stocks": int(player["stocks"] or 4),
            "team": player["team"],
            "cpu_level": int(player["cpu_level"] or 0),
            # The level byte a controller slot carries (wire.h
            # MSL_CORE_HUMAN_LEVEL); a human's Nana sets her AI up from it.
            "human_level": (levels["P1P2P3P4".index(str(player["port"])) // 2]
                            if levels is not None and
                            str(player["type"]) == "Human" else 0),
            "pre_seed": column(pre, "random_seed"),
            "buttons": column(pre, "buttons_physical"),
            "raw_x": column(pre, "raw_analog_x"),
            "raw_y": (column(pre, "raw_analog_y")
                      if "raw_analog_y" in pre_names else None),
            "raw_cx": (column(pre, "raw_analog_cstick_x")
                       if "raw_analog_cstick_x" in pre_names else None),
            "raw_cy": (column(pre, "raw_analog_cstick_y")
                       if "raw_analog_cstick_y" in pre_names else None),
            "joy_x": column(pre.field("joystick"), "x"),
            "joy_y": column(pre.field("joystick"), "y"),
            "c_x": column(pre.field("cstick"), "x"),
            "c_y": column(pre.field("cstick"), "y"),
            "trig_l": column(pre.field("triggers_physical"), "l"),
            "trig_r": column(pre.field("triggers_physical"), "r"),
            # The core's match config takes the internal character kind, which
            # only the post rows carry.
            "internal_char": column(post, "character"),
            "post_x": column(post.field("position"), "x"),
            "post_y": column(post.field("position"), "y"),
            "percent": column(post, "percent"),
            "state": column(post, "state"),
            "stocks_now": column(post, "stocks"),
            "direction": column(post, "direction"),
            "shield": column(post, "shield"),
            "valid": leader.is_valid().to_numpy(zero_copy_only=False),
        })
        # Ice Climbers: Nana is the port's follower, recorded with rows of her
        # own only while she is awake (asleep between her death and Popo's
        # respawn). She takes no inputs from the file - the core runs her CPU
        # mimic of Popo - so her rows are only ever compared, never driven.
        port_node = ports.field(index)
        if "follower" in port_node.type.names:
            follower = port_node.field("follower")
            fpost = follower.field("post")
            fvalid = (follower.is_valid().to_numpy(zero_copy_only=False) &
                      fpost.is_valid().to_numpy(zero_copy_only=False) &
                      fpost.field("character").is_valid()
                      .to_numpy(zero_copy_only=False))
            players[-1].update({
                "f_valid": fvalid,
                "f_x": column(fpost.field("position"), "x"),
                "f_y": column(fpost.field("position"), "y"),
                "f_percent": column(fpost, "percent"),
                "f_state": column(fpost, "state"),
                "f_stocks": column(fpost, "stocks"),
                "f_direction": column(fpost, "direction"),
                "f_shield": column(fpost, "shield"),
            })
    if frame_seed is None:
        frame_seed = np.asarray(players[0]["pre_seed"]).copy()
    keep, discarded_tail = finalized_rows(ids)
    duplicates = len(ids) - len(keep)
    if duplicates:
        rows = len(ids)
        ids = ids[keep]
        frame_seed = frame_seed[keep]
        for player in players:
            for name, value in list(player.items()):
                if isinstance(value, np.ndarray) and value.shape[:1] == (rows,):
                    player[name] = value[keep]
    items = None
    if "item" in names:
        node = frames.field("item")
        offsets = node.offsets.to_numpy(zero_copy_only=False)
        values = node.values
        velocity, position = values.field("velocity"), values.field("position")
        # Older recordings lack some lanes (misc from 3.2, owner from 3.6,
        # instance_id from 3.16); a missing lane is not compared.
        item_names = set(values.type.names)

        def lane(name):
            return column(values, name) if name in item_names else None

        misc = values.field("misc") if "misc" in item_names else None
        items = {
            "start": offsets[keep], "count": (offsets[1:] - offsets[:-1])[keep],
            "type": column(values, "type"), "state": column(values, "state"),
            "owner": lane("owner"),
            "direction": column(values, "direction"),
            "vel_x": column(velocity, "x"), "vel_y": column(velocity, "y"),
            "pos_x": column(position, "x"), "pos_y": column(position, "y"),
            "damage": column(values, "damage"), "timer": column(values, "timer"),
            "spawn_id": column(values, "id"),
            "misc": (None if misc is None else
                     np.stack([column(misc, str(k)) for k in range(4)], axis=1)),
            "instance_id": lane("instance_id"),
        }
    version = tuple(int(v) for v in start["slippi"]["version"])
    with open(path, "rb") as handle:
        raw = handle.read()
    stadium = stadium_transformations(start, raw)
    ucf_profile = recorded_profile(raw)
    from tools.validation.admission import gecko_codes, slippi_patches
    try:
        codes = gecko_codes(raw)
    except ValueError:
        codes = {}
    # Slippi 3.18+ records each change of the transformation phase.
    stadium_events = None
    if "stadium_transformation" in names:
        node = frames.field("stadium_transformation")
        offsets = node.offsets.to_numpy(zero_copy_only=False)
        values = node.values
        phases = column(values, "event")
        kinds = column(values, "type")
        # The state after each frame, carried forward from the last event;
        # None before the first (the stage's first proc, frame -39).
        stadium_events = [None] * len(keep)
        current = None
        for index, row in enumerate(keep):
            for k in range(int(offsets[row]), int(offsets[row + 1])):
                current = (int(phases[k]), int(kinds[k]))
            stadium_events[index] = current
    return {
        "path": str(path),
        "version": version,
        "stadium": stadium,
        "ucf_profile": ucf_profile,
        # wire.h MSL_PATCH_* (admission.slippi_patches): among them, a list
        # without NeutralSpawn's gecko at 0x8016E510 (MSL_PATCH_RETAIL_SPAWNS)
        # puts the fighters at their own port's stage spawn point.
        "slippi_patches": slippi_patches(codes),
        # Gameplay codes that came and went across Slippi versions, read
        # from the list when there is one: Online/Core/BrawlOffscreenDamage
        # (0x8006A880), Online/Core/FreezeDeadUpFallPhysics (0x800D4C1C) and
        # the Whispy blow-direction fix (0x8008653C, online and console).
        "code_flags": None if not codes else {
            "brawl_offscreen_damage": 0xC206A880 in codes,
            "freeze_dead_up_fall_physics": 0xC20D4C1C in codes,
            "whispy_dead_fighter_fix": 0xC208653C in codes,
        },
        "stadium_events": stadium_events,
        "items": items,
        "stage": int(start["stage"]),
        # -1 is items off; anything else spawns items at random, which the
        # simulator does not model (admission: unsupported-settings).
        "item_spawn_frequency": start.get("item_spawn_frequency", -1),
        "ids": ids,
        "frame_seed": frame_seed,
        "players": players,
        "is_teams": bool(start["is_teams"]),
        "damage_ratio": float(start["damage_ratio"]),
        "initial_seed": int(start["random_seed"]),
        # Scene major 8 proves the Slippi online code set owns this match.
        "online": int((start.get("scene") or {}).get("major") or 0) == 8,
        "fod": frames.field("fod_platform") if "fod_platform" in names else None,
        "whispy": (frames.field("dreamland_whispy")
                   if "dreamland_whispy" in names else None),
        "rows": keep,
        "duplicate_rows": duplicates,
        "discarded_tail": discarded_tail,
    }


def event_list(node, fields):
    offsets = node.offsets.to_numpy(zero_copy_only=False)
    values = node.values
    return offsets, [values.field(name).to_numpy(zero_copy_only=False)
                     for name in fields]


def events_at(offsets, values, index):
    for position in range(int(offsets[index]), int(offsets[index + 1])):
        yield tuple(value[position] for value in values)


GATE_SNAP, GATE_EXACT = "snap", "exact"


def reconstructed_cstick(player, gate=GATE_SNAP):
    """Raw C-stick bytes for a recording that has only the processed axes
    (Slippi before 3.17).

    The raw bytes feed one consumer: UCF 1.0's cardinal snap, which turns a
    C-stick at or past 80 on one axis, with the other within 6, into exactly
    +-1.0. Everything else reads the processed axes, which the recording has.
    The processed value alone cannot say whether the snap applied: +-0.9875
    with the other axis 0 comes from a raw 79 (no snap) or from a raw past 80
    that the stick clamp scaled back with a small off-axis value (snap). On 40
    Slippi 3.17 games that record both, the two are about even. GATE_SNAP
    reconstructs that case as 80, GATE_EXACT as 79; every other value is
    unambiguous and reconstructed as recorded."""
    cx = np.asarray(player["c_x"], dtype=np.float64)
    cy = np.asarray(player["c_y"], dtype=np.float64)
    raw_cx = np.clip(np.trunc(cx * 80.0), -80, 80)
    raw_cy = np.clip(np.trunc(cy * 80.0), -80, 80)
    if gate == GATE_SNAP:
        at_x = (np.abs(np.abs(cx) - 0.9875) < 1e-6) & (cy == 0)
        at_y = (np.abs(np.abs(cy) - 0.9875) < 1e-6) & (cx == 0)
        raw_cx[at_x] = np.sign(cx[at_x]) * 80
        raw_cy[at_y] = np.sign(cy[at_y]) * 80
    return raw_cx.astype(np.int16), raw_cy.astype(np.int16)


def cstick_gate_segments(player):
    """Runs of frames where a recording without raw C-stick bytes holds the
    C-stick at +-0.9875 on one axis with the other at 0 - the one reading
    reconstructed_cstick cannot settle. Each run is one stick position, so
    one choice: [(first index, last index, axis)]."""
    cx = np.asarray(player["c_x"], dtype=np.float64)
    cy = np.asarray(player["c_y"], dtype=np.float64)
    at = np.where((np.abs(np.abs(cx) - 0.9875) < 1e-6) & (cy == 0),
                  np.sign(cx), 0) +         np.where((np.abs(np.abs(cy) - 0.9875) < 1e-6) & (cx == 0),
                 2 * np.sign(cy), 0)
    runs = []
    start = None
    for i in range(len(at) + 1):
        value = at[i] if i < len(at) else 0
        if start is not None and value != at[start]:
            runs.append((start, i - 1, int(at[start])))
            start = None
        if start is None and value != 0:
            start = i
    return runs


def first_difference(replay, blob):
    for index in range(len(blob) // OUT_SIZE):
        if frame_diff(replay, blob, index):
            return index
    return None


FIT_REACH = 2
LAST_DRIVES = 0     # drives the last drive_fitted call ran (for timing)


def drive_fitted(replay, count, profile, tries=4, rounds=64):
    """drive(), deciding each ambiguous C-stick gate reading against the
    recording.

    Only for recordings without raw C-stick bytes, and only where the choice
    shows. Every reading starts unsnapped (a raw 79). At each first
    difference, the held runs that reach it are tried: the whole run
    toggled, then only its frames next to the difference, one at a time,
    since a stick can sit past the gate for part of a run and on it for the
    rest. A toggle is kept when it moves the first difference later.
    This is fitting to the recording, like the UCF profile fit, and carries
    the same caveat (module docstring)."""
    global LAST_DRIVES
    lanes = [p for p in replay["players"] if p["raw_cx"] is None]
    if not lanes:
        LAST_DRIVES = 1
        return drive(replay, count, profile), 0
    segments = []
    for lane, player in enumerate(replay["players"]):
        if player["raw_cx"] is None:
            player["raw_c_fit"] = reconstructed_cstick(player, GATE_EXACT)
            segments += [(first, last, axis, lane) for first, last, axis
                         in cstick_gate_segments(player)]
    segments.sort()
    flips = 0
    blob = drive(replay, count, profile)
    LAST_DRIVES = 1
    first = first_difference(replay, blob)
    tried = set()
    for _ in range(rounds):
        if first is None:
            break
        candidates = [seg for seg in segments
                      if seg[0] <= first and seg[1] >= first - FIT_REACH]
        improved = False
        for seg in list(reversed(candidates))[:tries]:
            player = replay["players"][seg[3]]
            raw_cx, raw_cy = player["raw_c_fit"]
            lane_raw = raw_cx if abs(seg[2]) == 1 else raw_cy
            gate = seg[2] if abs(seg[2]) == 1 else seg[2] // 2
            tail = (max(seg[0], first - FIT_REACH), min(seg[1], first))
            spans = [(seg[0], seg[1])]
            if tail != spans[0] and tail[0] <= tail[1]:
                spans.append(tail)
            spans += [(i, i) for i in range(tail[1], tail[0] - 1, -1)
                      if (i, i) not in spans]
            for lo, hi in spans:
                key = (seg[3], seg[2], lo, hi, first)
                if key in tried:
                    continue
                tried.add(key)
                saved = lane_raw.copy()
                span = slice(lo, hi + 1)
                # Toggle: a span now unsnapped becomes snapped and the
                # other way round.
                snapped = abs(int(lane_raw[lo])) >= 80
                lane_raw[span] = gate * (79 if snapped else 80)
                trial = drive(replay, count, profile)
                LAST_DRIVES += 1
                moved = first_difference(replay, trial)
                if moved is None or moved > first:
                    blob, first, improved = trial, moved, True
                    flips += 1
                    break
                lane_raw[:] = saved
            if improved:
                break
        if not improved:
            break
    return blob, flips


# wire.h MSL_JIT_*: the capturing Dolphin's host arithmetic, in the order
# drive_arithmetic tries them.
JIT_ARITHMETIC = (0, 1)     # FMA3 host, then MSL_JIT_NO_HOST_FMA


def drive_arithmetic(replay, count, profile):
    """drive_fitted() under each host arithmetic the capturing Dolphin could
    have run, keeping the one that matches furthest (replay["jit_arithmetic"]
    is left at it).

    The recording does not say whether the capturing machine had FMA3: a
    Dolphin without it rounds the product of a double-precision fmadd before
    the add, which moves the last bit of a sqrtf or a collision intersection
    now and then. Like the UCF profile, this is a fit to the recording, and a
    second drive only runs when the first one differs."""
    global LAST_DRIVES
    best = None
    drives = 0
    for jit in JIT_ARITHMETIC:
        replay["jit_arithmetic"] = jit
        blob, flips = drive_fitted(replay, count, profile)
        drives += LAST_DRIVES
        first = first_difference(replay, blob)
        reach = len(blob) if first is None else first
        if best is None or reach > best[0]:
            best = (reach, jit, blob, flips)
        if first is None:
            break
    replay["jit_arithmetic"] = best[1]
    LAST_DRIVES = drives
    return best[2], best[3]


def build_tape(replay, count, profile="ucf10", raw_cstick=True, raw_main=True,
               cstick_gate=GATE_SNAP):
    players = replay["players"]
    ucf = profile_bits(profile)
    first = int(replay["ids"][0])
    online = 1 if replay["online"] else 0
    flags = replay.get("code_flags")
    offscreen = int(flags["brawl_offscreen_damage"]) if flags else online
    dead_up_fall = (int(flags["freeze_dead_up_fall_physics"]) if flags
                    else online)
    whispy_fix = int(flags["whispy_dead_fighter_fix"]) if flags else online
    streams = ((1 if replay["fod"] is not None else 0) |
               (2 if replay["whispy"] is not None else 0))
    header = struct.pack(
        CONFIG_FMT,
        TAPE_MAGIC,
        replay["stage"],
        first - 1,                       # native.c: first finalized row - 1
        int(replay["frame_seed"][0]),
        replay["initial_seed"],
        replay["damage_ratio"],
        len(players), 1 if replay["is_teams"] else 0, 0,
        max(p["stocks"] for p in players),
        online, offscreen, dead_up_fall, whispy_fix,
        ucf[0], ucf[1], ucf[2], ucf[3], ucf[4],
        streams, replay.get("stadium", 0) or 0,
        # The profile's absent UCF routines replace the list's (a fitted
        # profile is tried against the recording like the other fields).
        (replay.get("slippi_patches", 0) & ~(7 << UCF_ABSENT_SHIFT)) |
        (ucf[5] << UCF_ABSENT_SHIFT),
        replay.get("jit_arithmetic", 0),
        *[value
          for index in range(4)
          for value in ((int(players[index]["internal_char"][0]),
                         0 if players[index]["team"] is None
                         else int(players[index]["team"]),
                         ((players[index]["port"] + 1) << 1) |
                         (1 if players[index]["direction"][0] > 0 else 0),
                         players[index]["costume"], players[index]["handicap"],
                         0, tape_level(players[index]), 0)
                        if index < len(players) else (0,) * 8)],
        count)

    lanes = []
    for player in players:
        # Physical bytes pass through as recorded: real controllers report
        # past +-80 and UCF's raw consumers read them unclamped. Only derived
        # axes take processed_stick_i8's truncation and clamp.
        raw_x = np.asarray(player["raw_x"], dtype=np.int16)
        if raw_main and player["raw_y"] is not None:
            raw_y = np.asarray(player["raw_y"], dtype=np.int16)
        else:
            raw_y = np.clip(np.trunc(np.asarray(player["joy_y"]) * 80.0),
                            -80, 80).astype(np.int16)
        if raw_cstick and player["raw_cx"] is not None:
            raw_cx = np.asarray(player["raw_cx"], dtype=np.int16)
            raw_cy = np.asarray(player["raw_cy"], dtype=np.int16)
        elif "raw_c_fit" in player:
            raw_cx, raw_cy = player["raw_c_fit"]
        else:
            raw_cx, raw_cy = reconstructed_cstick(player, cstick_gate)
        lanes.append({
            "buttons": np.asarray(player["buttons"], dtype=np.uint16),
            "main_x": raw_x, "main_y": raw_y, "c_x": raw_cx, "c_y": raw_cy,
            "l": trigger_u8(player["trig_l"]),
            "r": trigger_u8(player["trig_r"]),
            "nml_main_x": np.asarray(player["joy_x"], dtype=np.float32),
            "nml_main_y": np.asarray(player["joy_y"], dtype=np.float32),
            "nml_c_x": np.asarray(player["c_x"], dtype=np.float32),
            "nml_c_y": np.asarray(player["c_y"], dtype=np.float32),
            "seed": player["pre_seed"],
            "valid": player["valid"],
        })

    fod = (event_list(replay["fod"], ("platform", "height"))
           if replay["fod"] is not None else (None, None))
    whispy = (event_list(replay["whispy"], ("direction",))
              if replay["whispy"] is not None else (None, None))
    blocks = [header]
    for index in range(count):
        source_row = int(replay["rows"][index]) if replay["duplicate_rows"] \
            else index
        seed_lane = next((lane for lane in lanes if lane["valid"][index]),
                         lanes[0])
        fod_left = fod_right = 0.0
        fod_valid = 0
        if fod[0] is not None:
            for platform, height in events_at(fod[0], fod[1], source_row):
                if int(platform) == 0:
                    fod_left, fod_valid = float(height), fod_valid | 1
                else:
                    fod_right, fod_valid = float(height), fod_valid | 2
        whispy_valid = whispy_direction = 0
        if whispy[0] is not None:
            for (direction,) in events_at(whispy[0], whispy[1], source_row):
                whispy_valid, whispy_direction = 1, int(direction)
        row = [int(replay["frame_seed"][index]), int(seed_lane["seed"][index]),
               1, fod_valid, whispy_valid, whispy_direction,
               fod_left, fod_right]
        for lane in range(4):
            if lane < len(lanes) and lanes[lane]["valid"][index]:
                source = lanes[lane]
                row += [int(source["buttons"][index]),
                        int(source["main_x"][index]),
                        int(source["main_y"][index]),
                        int(source["c_x"][index]), int(source["c_y"][index]),
                        int(source["l"][index]), int(source["r"][index]), 3,
                        float(source["nml_main_x"][index]),
                        float(source["nml_main_y"][index]),
                        float(source["nml_c_x"][index]),
                        float(source["nml_c_y"][index])]
            else:
                row += [0] * 8 + [0.0] * 4
        blocks.append(struct.pack(FRAME_FMT, *row))
    return b"".join(blocks)


def drive(replay, count, profile="ucf10", raw_cstick=True, raw_main=True,
          cstick_gate=GATE_SNAP):
    """Run the core over the tape; returns the raw per-frame output block."""
    tape = build_tape(replay, count, profile, raw_cstick, raw_main, cstick_gate)
    with tempfile.TemporaryDirectory(prefix="msl_tape_") as folder:
        tape_path = os.path.join(folder, "tape.bin")
        out_path = os.path.join(folder, "out.bin")
        with open(tape_path, "wb") as handle:
            handle.write(tape)
        result = subprocess.run(
            [str(DRIVER), str(DATA_ROOT), tape_path, out_path],
            capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip()[:300] or "driver failed")
        with open(out_path, "rb") as handle:
            return handle.read()


ITEM_BASE = 101         # first item lane in an unpacked OutFrame row
ITEM_NAMES = ("exists", "state", "owner", "misc_mask", "type", "instance_id",
              "misc0", "misc1", "misc2", "misc3", "damage", "pad", "spawn_id",
              "direction", "vel_x", "vel_y", "pos_x", "pos_y", "timer")


def frame_state(blob, index):
    row = struct.unpack_from(OUT_FMT, blob, index * OUT_SIZE)
    items = []
    for slot in range(row[78]):
        base = ITEM_BASE + slot * len(ITEM_NAMES)
        items.append(dict(zip(ITEM_NAMES, row[base:base + len(ITEM_NAMES)])))
    return {
        "frame_id": row[0], "pos_x": row[2:6], "pos_y": row[6:10],
        "percent": row[10:14], "shield_hp": row[14:18],
        "action_id": row[18:22], "action_frame": row[22:26],
        "hitstun": row[26:30], "hitlag": row[30:34], "stocks": row[34:38],
        "facing": row[38:42], "on_ground": row[42:46], "is_dead": row[46:50],
        "jumps_left": row[50:54], "hurtbox_state": row[54:58],
        "follower_present": row[66:70], "follower_facing": row[70:74],
        "follower_stocks": row[74:78],
        "stadium_phase": row[79], "stadium_kind": row[80],
        "follower_action_id": row[81:85],
        "follower_pos_x": row[85:89], "follower_pos_y": row[89:93],
        "follower_percent": row[93:97], "follower_shield_hp": row[97:101],
        "items": items,
    }


# Item lanes compared like tools/validation/native.c::compare_row: every
# recorded lane, the misc bytes only where both sides say they are gameplay
# state, and the instance id only where the recording has one (Slippi 3.16).
ITEM_COMPARED = ("type", "state", "owner", "direction", "vel_x", "vel_y",
                 "pos_x", "pos_y", "damage", "timer", "spawn_id")


def recorded_items(replay, index):
    items = replay.get("items")
    if items is None:
        return None
    start = int(items["start"][index])
    count = min(int(items["count"][index]), MAX_ITEMS)
    return [start + k for k in range(count)]


def frame_diff(replay, blob, index):
    """[(field, player, simulated, recorded)] for one row; empty means equal.

    Both ports' fighters, Nana where a port has her, and every item. A
    follower mismatch names its field `follower.<field>` and an item one
    `item.<field>` with the item's slot in place of the player."""
    got = frame_state(blob, index)
    bad = []
    for lane, player in enumerate(replay["players"]):
        if not player["valid"][index]:
            continue
        want = {
            "pos_x": float(player["post_x"][index]),
            "pos_y": float(player["post_y"][index]),
            "percent": float(player["percent"][index]),
            "shield_hp": float(player["shield"][index]),
            "action_id": int(player["state"][index]),
            "stocks": int(player["stocks_now"][index]),
            "facing": 1 if player["direction"][index] > 0 else 0,
        }
        for name, value in want.items():
            if float(got[name][lane]) != float(value):
                bad.append((name, lane, float(got[name][lane]), float(value)))
        if "f_valid" not in player:
            continue
        recorded = bool(player["f_valid"][index])
        if recorded != bool(got["follower_present"][lane]):
            bad.append(("follower.present", lane,
                        float(got["follower_present"][lane]), float(recorded)))
            continue
        if not recorded:
            continue
        want = {
            "pos_x": float(player["f_x"][index]),
            "pos_y": float(player["f_y"][index]),
            "percent": float(player["f_percent"][index]),
            "shield_hp": float(player["f_shield"][index]),
            "action_id": int(player["f_state"][index]),
            "stocks": int(player["f_stocks"][index]),
            "facing": 1 if player["f_direction"][index] > 0 else 0,
        }
        for name, value in want.items():
            simulated = got["follower_" + name][lane]
            if float(simulated) != float(value):
                bad.append(("follower." + name, lane, float(simulated),
                            float(value)))
    rows = recorded_items(replay, index)
    if rows is None:
        return bad
    items = replay["items"]
    if len(rows) != len(got["items"]):
        bad.append(("item.count", -1, float(len(got["items"])),
                    float(len(rows))))
        return bad
    with_instance = (items["instance_id"] is not None and
                     tuple(replay.get("version", (0,))) >= (3, 16, 0))
    for slot, (row, sim) in enumerate(zip(rows, got["items"])):
        for name in ITEM_COMPARED:
            if items[name] is None:
                continue
            if float(sim[name]) != float(items[name][row]):
                bad.append(("item." + name, slot, float(sim[name]),
                            float(items[name][row])))
        if with_instance and sim["instance_id"] != int(items["instance_id"][row]):
            bad.append(("item.instance_id", slot, float(sim["instance_id"]),
                        float(items["instance_id"][row])))
        for k in range(4 if items["misc"] is not None else 0):
            # The driver's mask is the simulated item's; the recorded item is
            # the same kind and state whenever those lanes matched above.
            if (sim["misc_mask"] >> k) & 1 and                     sim["misc%d" % k] != int(items["misc"][row][k]):
                bad.append(("item.misc%d" % k, slot, float(sim["misc%d" % k]),
                            float(items["misc"][row][k])))
    return bad


def stadium_diff(replay, blob, index):
    """Where the recording carries Stadium transformation events (3.18+):
    (simulated, recorded) (phase, kind) when they disagree, else None. The
    event is the state after the stage's frame, as the simulator reports it."""
    events = replay.get("stadium_events")
    if not events or events[index] is None:
        return None
    got = frame_state(blob, index)
    simulated = (int(got["stadium_phase"]), int(got["stadium_kind"]))
    return None if simulated == events[index] else (simulated, events[index])


def supported(replay) -> str | None:
    """Why this recording cannot be driven, or None."""
    if len(replay["players"]) != 2:
        return "not-singles"
    if replay["stage"] not in LEGAL_STAGES:
        return "stage-%d" % replay["stage"]
    if replay.get("stadium", 0) is None:
        return "stadium-unpatched-transformations"
    if replay.get("item_spawn_frequency", -1) not in (-1, None):
        return "items-on"
    # A match starts every fighter at 0%. A first row with damage already on
    # it is a recording that does not begin at the match's start (seen on a
    # ranked file starting at 27% and 78% with a 453-second timer).
    if any(len(p.get("percent", ())) and float(p["percent"][0]) != 0.0
           for p in replay["players"]):
        return "not-from-match-start"
    return None


def state_matches_recording(path, through=None, profiles=None,
                            raw_cstick=True):
  """Does the simulator land where this recording did, up to `through`?

  Not a validation verdict and not admission - see the module docstring. The
  answer is a dict with `matches`, the `fitted_inputs_profile` that produced
  it, the frames driven, the seconds it took, and `note`, which says what the
  answer is not. `through` stops at a frame id (the takeover frame, say)
  rather than running the whole game.
  """
  replay = read(path)
  reason = supported(replay)
  if reason:
    return {'matches': False, 'reason': reason,
            'fitted_inputs_profile': None, 'note': NOT_A_VERDICT}
  index_of = {int(frame): i for i, frame in enumerate(replay['ids'])}
  if through is None:
    count = len(replay['ids'])
  else:
    if int(through) not in index_of:
      return {'matches': False, 'reason': 'frame-not-in-file',
              'fitted_inputs_profile': None, 'note': NOT_A_VERDICT}
    count = index_of[int(through)] + 1
  started = time.time()
  attempts = []
  if profiles is None:
    profiles = candidate_profiles(replay)
  for profile in profiles:
    if raw_cstick:
      blob, _ = drive_arithmetic(replay, count, profile)
    else:
      blob = drive(replay, count, profile, raw_cstick=False)
    rows = len(blob) // OUT_SIZE
    first_bad = None
    for index in range(rows):
      if frame_diff(replay, blob, index):
        first_bad = int(replay['ids'][index])
        break
    attempts.append({'profile': profile, 'first_difference': first_bad})
    if first_bad is None:
      return {'matches': True, 'reason': None,
              'fitted_inputs_profile': profile, 'frames': rows,
              'seconds': round(time.time() - started, 3),
              'attempts': attempts, 'blob': blob, 'replay': replay,
              'note': NOT_A_VERDICT}
  return {'matches': False, 'reason': 'differs', 'fitted_inputs_profile': None,
          'frames': count, 'seconds': round(time.time() - started, 3),
          'attempts': attempts, 'note': NOT_A_VERDICT}


def summarize(path, profile="ucf10", raw_cstick=True, limit=None):
    replay = read(path)
    reason = supported(replay)
    if reason:
        return {"path": str(path), "skipped": reason}
    count = len(replay["ids"]) if limit is None else min(len(replay["ids"]),
                                                         limit + 123)
    started = time.time()
    blob = drive(replay, count, profile, raw_cstick=raw_cstick)
    elapsed = time.time() - started
    rows = len(blob) // OUT_SIZE
    exact = 0
    first_bad = None
    fields = {}
    for index in range(rows):
        bad = frame_diff(replay, blob, index)
        if not bad:
            exact += 1
            continue
        frame_id = int(replay["ids"][index])
        if first_bad is None:
            first_bad = frame_id
        for name, _lane, got, want in bad:
            entry = fields.setdefault(name, {"first_frame": frame_id,
                                             "count": 0, "max_abs": 0.0})
            entry["count"] += 1
            entry["max_abs"] = max(entry["max_abs"], abs(got - want))
    return {
        "path": str(path),
        "profile": profile,
        "frames": rows,
        "seconds": round(elapsed, 3),
        "fps": round(rows / elapsed) if elapsed else None,
        "stage": replay["stage"],
        "characters": [p["character"] for p in replay["players"]],
        "raw_cstick": replay["players"][0]["raw_cx"] is not None,
        "duplicate_rows": replay["duplicate_rows"],
        "exact": first_bad is None,
        "exact_frames": exact,
        "exact_fraction": round(exact / max(rows, 1), 5),
        "first_divergence": first_bad,
        "fields": fields,
    }


# Viewer track (runtime/viewer_track.h): event types the core writes, and the
# ones a replay viewer might want that the headless core does not run.
VIEWER_TRACK_VERSION = 1
VIEWER_TRACK_TYPES = ("fx", "fxend", "gen", "tint", "charge", "part", "pos",
                      "shake", "hide", "ganim", "bg", "fd", "gcolor", "bgcolor",
                      "cam")
VIEWER_TRACK_MISSING = {
    "gen (fighter)": "only joint-animation particle events from stage "
                     "animations are recorded; fighter animations' particle "
                     "events are not run",
    "gjoint": "code-moved stage joints (Dream Land's birds) are not run",
}


_TRACK_BUILD = {}


def viewer_track_build() -> int:
    """sim_build for the viewer track: the first 48 bits of the SHA-256 of
    the driver executable with its PE link timestamp and checksum zeroed, so
    it changes only when the code does, not on every relink. Cached per
    path, size and modification time."""
    stat = DRIVER.stat()
    key = (str(DRIVER), stat.st_size, stat.st_mtime_ns)
    if key not in _TRACK_BUILD:
        import hashlib
        image = bytearray(DRIVER.read_bytes())
        if image[:2] == b"MZ":
            pe = struct.unpack_from("<I", image, 0x3C)[0]
            if image[pe:pe + 4] == b"PE\0\0":
                image[pe + 8:pe + 12] = bytes(4)          # TimeDateStamp
                image[pe + 88:pe + 92] = bytes(4)         # CheckSum
        digest = hashlib.sha256(image).hexdigest()
        _TRACK_BUILD.clear()
        _TRACK_BUILD[key] = int(digest[:12], 16)
    return _TRACK_BUILD[key]


def export_viewer_track(path, out_json):
    """Replay `path` through the core and write its viewer track to
    `out_json`: {"version", "sim_build", "exact", "events"}, each event
    [frame, type, ...values] with frame in the game's numbering (-123 first).

    The inputs are fitted the way state_matches_recording() fits them (the
    recording's UCF profile, the C-stick gate readings, the capture's host
    arithmetic); a final drive with those inputs writes the track. "exact"
    says whether that drive reproduced the recording. Returns the dict
    written, plus "seconds"."""
    started = time.time()
    replay = read(path)
    reason = supported(replay)
    if reason:
        raise ValueError("not supported: %s" % reason)
    count = len(replay["ids"])
    own = replay.get("ucf_profile")
    profiles = [own] if own else list(candidate_profiles(replay))
    chosen = None
    first = None
    for profile in profiles:
        blob, _ = drive_arithmetic(replay, count, profile)
        first = first_difference(replay, blob)
        chosen = profile
        if first is None:
            break
    with tempfile.TemporaryDirectory(prefix="msl_track_") as folder:
        track_path = os.path.join(folder, "track.jsonl")
        previous = os.environ.get("MSL_VIEWER_TRACK")
        os.environ["MSL_VIEWER_TRACK"] = track_path
        try:
            blob = drive(replay, count, chosen)
        finally:
            if previous is None:
                del os.environ["MSL_VIEWER_TRACK"]
            else:
                os.environ["MSL_VIEWER_TRACK"] = previous
        events = []
        with open(track_path, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line:
                    event = json.loads(line)
                    # The core writes [frame, type, ...]; the file is
                    # [frame, type, ...] as well.
                    events.append(event)
    events.sort(key=lambda event: event[0])
    result = {
        "version": VIEWER_TRACK_VERSION,
        "sim_build": viewer_track_build(),
        "exact": first_difference(replay, blob) is None,
        "events": events,
    }
    with open(out_json, "w", encoding="utf-8") as handle:
        json.dump(result, handle, separators=(",", ":"))
    result["seconds"] = round(time.time() - started, 3)
    return result


def main() -> None:
    args = list(sys.argv[1:])
    if args[:1] == ["--viewer-track"]:
        result = export_viewer_track(args[1], args[2])
        print(json.dumps({"events": len(result["events"]),
                          "exact": result["exact"],
                          "seconds": result["seconds"]}))
        return
    out = profile = None
    limit = None
    raw_cstick = "--no-raw-cstick" not in args
    if not raw_cstick:
        args.remove("--no-raw-cstick")
    for flag, setter in (("--json", "out"), ("--profile", "profile"),
                         ("--frames", "frames")):
        if flag in args:
            index = args.index(flag)
            value = args[index + 1]
            del args[index:index + 2]
            if setter == "out":
                out = value
            elif setter == "profile":
                profile = value
            else:
                limit = int(value)
    paths = ([line.strip() for line in open(args[1]) if line.strip()]
             if args and args[0] == "--list" else args)
    if not available():
        print("replay driver or extracted data is missing: %s" % DRIVER)
        raise SystemExit(2)
    results = []
    for path in paths:
        try:
            if profile:
                result = summarize(path, profile, raw_cstick, limit)
            else:
                fit = state_matches_recording(path, raw_cstick=raw_cstick)
                result = summarize(path, fit["fitted_inputs_profile"]
                                   or PROFILE_ORDER[0], raw_cstick, limit)
                result["fitted_inputs_profile"] = fit["fitted_inputs_profile"]
        except Exception as error:  # noqa: BLE001
            result = {"path": str(path),
                      "error": "%s: %s" % (type(error).__name__, error)}
        results.append(result)
        print(json.dumps({k: v for k, v in result.items() if k != "fields"}),
              flush=True)
        if result.get("fields"):
            print("   ", json.dumps(result["fields"]), flush=True)
    if out:
        with open(out, "w") as handle:
            json.dump(results, handle, indent=1)


if __name__ == "__main__":
    main()

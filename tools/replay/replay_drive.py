"""Replay a .slp through the core and diff it against the recording.

Builds the tape `replay_drive` consumes - match config, per-frame recorded
inputs, the frame-start RNG, the fighter RNG and the stage events - runs it,
and compares every frame against the file.

  python -m tools.replay.replay_drive <replay.slp> [...] [--json out.json]
  python -m tools.replay.replay_drive --list files.txt --profile ucf084

**This is not validation and its answers are not verdicts.** The strict gate
lives in tools/validation; admission is what decides whether a recording may
be compared at all, and agent_docs/VALIDATION.md forbids exactly what
`state_matches_recording` does: it tries each UCF capability profile and keeps
the one that reproduces the file, where the gate requires reviewed
provenance and says "never pick a profile solely because it makes a recording
pass". Nothing here may be used to admit a capture, record a suite result,
write an output lock, or claim that the simulator is or is not exact. It
answers one question for one consumer: may I start something from the
position this recording reached at this frame?

Two things are per-recording and have to be right or nothing matches:

  * the internal character id, from the post-frame rows - the external id in
    the game-start block picks another character's entry data;
  * the UCF capability profile, fitted as described above.

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

CONFIG_FMT = "<IiiIIf16B32BI"
FRAME_FMT = "<II4B2f" + "H4b3B4f" * 4
OUT_FMT = "<iI16f4H4h4H4H32B"
CONFIG_SIZE = struct.calcsize(CONFIG_FMT)
FRAME_SIZE = struct.calcsize(FRAME_FMT)
OUT_SIZE = struct.calcsize(OUT_FMT)
TAPE_MAGIC = 0x314C534D  # "MSL1"

LEGAL_STAGES = (2, 3, 8, 28, 31, 32)

NOT_A_VERDICT = ('not admission and not a validation result: the inputs '
                 'profile is fitted by trying each one, which the strict '
                 'gate forbids (agent_docs/VALIDATION.md)')

# cardinals, shield SDI, SDI, extended shield drop, 0.84 shield drop.
UCF_PROFILES = {
    "ucf10": (1, 1, 1, 1, 1),
    "ucf084": (0, 0, 0, 1, 1),
}
PROFILE_ORDER = ("ucf10", "ucf084")

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


def read(path):
    """Everything the tape needs plus the per-frame truth to compare against."""
    game = _read_slippi(str(path), False)
    start = game.start
    frames = game.frames
    names = frames.type.names
    ids = column(frames, "id")
    frame_seed = column(frames.field("start"), "random_seed")
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
    order = np.argsort(ids, kind="stable")
    keep = [order[i] for i in range(len(order))
            if i + 1 == len(order) or ids[order[i + 1]] != ids[order[i]]]
    keep = np.asarray(keep, dtype=np.int64)
    duplicates = len(order) - len(keep)
    if duplicates:
        rows = len(ids)
        ids = ids[keep]
        frame_seed = frame_seed[keep]
        for player in players:
            for name, value in list(player.items()):
                if isinstance(value, np.ndarray) and value.shape[:1] == (rows,):
                    player[name] = value[keep]
    return {
        "path": str(path),
        "stage": int(start["stage"]),
        "ids": ids,
        "frame_seed": frame_seed,
        "players": players,
        "is_teams": bool(start["is_teams"]),
        "damage_ratio": float(start["damage_ratio"]),
        "initial_seed": int(start["random_seed"]),
        # Scene major 8 proves the Slippi online code set owns this match.
        "online": int(start["scene"]["major"]) == 8,
        "fod": frames.field("fod_platform") if "fod_platform" in names else None,
        "whispy": (frames.field("dreamland_whispy")
                   if "dreamland_whispy" in names else None),
        "rows": keep,
        "duplicate_rows": duplicates,
    }


def event_list(node, fields):
    offsets = node.offsets.to_numpy(zero_copy_only=False)
    values = node.values
    return offsets, [values.field(name).to_numpy(zero_copy_only=False)
                     for name in fields]


def events_at(offsets, values, index):
    for position in range(int(offsets[index]), int(offsets[index + 1])):
        yield tuple(value[position] for value in values)


def build_tape(replay, count, profile="ucf10", raw_cstick=True, raw_main=True):
    players = replay["players"]
    ucf = UCF_PROFILES[profile]
    first = int(replay["ids"][0])
    online = 1 if replay["online"] else 0
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
        online, online, online, online,
        ucf[0], ucf[1], ucf[2], ucf[3], ucf[4],
        streams, 0, 0,
        *[value
          for index in range(4)
          for value in ((int(players[index]["internal_char"][0]),
                         0 if players[index]["team"] is None
                         else int(players[index]["team"]),
                         ((players[index]["port"] + 1) << 1) |
                         (1 if players[index]["direction"][0] > 0 else 0),
                         players[index]["costume"], players[index]["handicap"],
                         0, players[index]["cpu_level"], 0)
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
        else:
            raw_cx = np.clip(np.trunc(np.asarray(player["c_x"]) * 80.0),
                             -80, 80).astype(np.int16)
            raw_cy = np.clip(np.trunc(np.asarray(player["c_y"]) * 80.0),
                             -80, 80).astype(np.int16)
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


def drive(replay, count, profile="ucf10", raw_cstick=True, raw_main=True):
    """Run the core over the tape; returns the raw per-frame output block."""
    tape = build_tape(replay, count, profile, raw_cstick, raw_main)
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


def frame_state(blob, index):
    row = struct.unpack_from(OUT_FMT, blob, index * OUT_SIZE)
    return {
        "frame_id": row[0], "pos_x": row[2:6], "pos_y": row[6:10],
        "percent": row[10:14], "shield_hp": row[14:18],
        "action_id": row[18:22], "action_frame": row[22:26],
        "hitstun": row[26:30], "hitlag": row[30:34], "stocks": row[34:38],
        "facing": row[38:42], "on_ground": row[42:46], "is_dead": row[46:50],
        "jumps_left": row[50:54], "hurtbox_state": row[54:58],
    }


def frame_diff(replay, blob, index):
    """[(field, player, simulated, recorded)] for one row; empty means equal."""
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
    return bad


def supported(replay) -> str | None:
    """Why this recording cannot be driven, or None."""
    if len(replay["players"]) != 2:
        return "not-singles"
    if replay["stage"] not in LEGAL_STAGES:
        return "stage-%d" % replay["stage"]
    return None


def state_matches_recording(path, through=None, profiles=PROFILE_ORDER,
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
  for profile in profiles:
    blob = drive(replay, count, profile, raw_cstick=raw_cstick)
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


def main() -> None:
    args = list(sys.argv[1:])
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

"""tools/replay/replay_drive: the replay lane's tape, output rows and checks.

Pure-Python pieces run everywhere; the driver smoke needs a built
replay_drive and extracted game data, and skips without them.
"""
from __future__ import annotations

import json
import struct
import subprocess

import numpy as np
import pytest

from tools.replay import replay_drive as rd


def test_output_row_layout_matches_the_driver():
    if not rd.DRIVER.is_file():
        pytest.skip("replay_drive is not built")
    sizes = json.loads(subprocess.run([str(rd.DRIVER), "--sizes"],
                                      capture_output=True, text=True,
                                      check=True).stdout)
    assert sizes["out"] == rd.OUT_SIZE
    assert sizes["config"] == rd.CONFIG_SIZE
    assert sizes["frame"] == rd.FRAME_SIZE


def _run_mode(args, steps, lanes, run_out, run_in):
    """A run-mode driver's rows: the start and one per step of `steps`."""
    proc = subprocess.Popen([str(rd.DRIVER), str(rd.DATA_ROOT)] + args,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    rows = [proc.stdout.read(run_out * lanes)]
    for step in steps:
        proc.stdin.write(step)
        proc.stdin.flush()
        rows.append(proc.stdout.read(run_out * lanes))
    proc.stdin.close()
    err = proc.stderr.read()
    assert proc.wait() == 0, err
    return rows


def test_a_saved_state_plays_as_the_replayed_one(tmp_path):
    # --save writes the position run mode forks its lanes from; --load starts
    # there without the tape: the same rows, step for step, lane for lane
    recordings = sorted((rd.ROOT / "replays" / "validation").rglob("*.slp"))
    if not rd.available() or not recordings:
        pytest.skip("replay_drive, the extracted data or a recording is missing")
    sizes = json.loads(subprocess.run([str(rd.DRIVER), "--sizes"],
                                      capture_output=True, text=True,
                                      check=True).stdout)
    if not sizes.get("save_load"):
        pytest.skip("this replay_drive has no --save/--load")
    replay = rd.read(recordings[0])
    count = min(900, len(replay["ids"]))
    tape = tmp_path / "tape.bin"
    tape.write_bytes(rd.build_tape(replay, count))
    state = tmp_path / "state.bin"
    lanes, run_out, run_in = 3, sizes["run_out"], sizes["run_in"]
    rng = np.random.default_rng(0)
    steps = [rng.integers(0, 256, run_in * lanes, dtype=np.uint8).tobytes()
             for _ in range(40)]
    replayed = _run_mode([str(tape), str(tmp_path / "a.out"), "--run", str(lanes),
                          "--save", str(state)], steps, lanes, run_out, run_in)
    loaded = _run_mode([str(tape), str(tmp_path / "b.out"), "--load", str(state),
                        "--run", str(lanes)], steps, lanes, run_out, run_in)
    assert state.stat().st_size > 0
    assert replayed == loaded
    # a state this build cannot take is exit code 3, for the caller to replay
    bad = tmp_path / "bad.bin"
    blob = bytearray(state.read_bytes())
    blob[100] ^= 0xFF
    bad.write_bytes(bytes(blob))
    got = subprocess.run([str(rd.DRIVER), str(rd.DATA_ROOT), str(tape),
                          str(tmp_path / "c.out"), "--load", str(bad), "--run", "1"],
                         capture_output=True, input=b"")
    assert got.returncode == 3


def test_recorded_profile_comes_first_and_fitted_ones_follow():
    replay = {"ucf_profile": "ucf:000000"}
    assert rd.candidate_profiles(replay) == (
        "ucf:000000", "ucf10", "ucf084", "ucf074", "vanilla")
    # A recorded profile equal to a fitted one is not tried twice.
    replay = {"ucf_profile": "ucf:111110"}
    assert rd.candidate_profiles(replay) == (
        "ucf:111110", "ucf084", "ucf08", "ucf074", "vanilla")
    assert rd.candidate_profiles({}) == rd.PROFILE_ORDER
    # The sixth field is the absent-code mask; the five-field form has none.
    assert rd.profile_bits("ucf:010113") == (0, 1, 0, 1, 1, 3)
    assert rd.profile_bits("ucf:01011") == (0, 1, 0, 1, 1, 0)
    assert rd.profile_bits("ucf08") == (0, 0, 0, 0, 0, 0)
    assert rd.profile_bits("vanilla") == (0, 0, 0, 0, 0, 7)


def test_recorded_profile_names_the_missing_ucf_codes(monkeypatch):
    # UCF 0.74's set: no tumble fix, the others present, the 312-byte drop.
    _codes(monkeypatch, {0xC20C9A44: b"", 0xC20998A4: bytes(312)})
    assert rd.recorded_profile(b"") == "ucf:000001"
    # Nothing UCF at all.
    _codes(monkeypatch, {0xC216E510: b""})
    assert rd.recorded_profile(b"") == "ucf:000007"
    _codes(monkeypatch, {})
    assert rd.recorded_profile(b"") is None


def test_fitting_decides_part_of_a_held_gate_run(monkeypatch):
    # A lane holds -0.9875 on x for frames 3..8. The console read a raw 80
    # (snapped) for 6..8 only. The fake drive diverges at the first frame
    # whose raw value differs from the console's.
    console = np.array([0, 0, 0, -79, -79, -79, -80, -80, -80, 0])
    c_x = np.where(console != 0, -0.9875, 0.0)
    player = {"raw_cx": None, "c_x": c_x, "c_y": np.zeros(10)}
    replay = {"players": [player]}

    def drive(replay, count, profile):
        return player["raw_c_fit"][0].copy()

    def first_difference(replay, blob):
        wrong = np.nonzero(blob != console)[0]
        return int(wrong[0]) + 1 if len(wrong) else None

    monkeypatch.setattr(rd, "drive", drive)
    monkeypatch.setattr(rd, "first_difference", first_difference)
    blob, flips = rd.drive_fitted(replay, 10, "ucf10")
    assert blob.tolist() == console.tolist()
    assert flips >= 1
    # A recording with raw C-stick bytes has nothing to fit.
    player["raw_cx"] = np.zeros(10)
    monkeypatch.setattr(rd, "drive", lambda replay, count, profile: "blob")
    assert rd.drive_fitted(replay, 10, "ucf10") == ("blob", 0)


def _codes(monkeypatch, codes, frozen=False):
    from tools.validation import admission
    monkeypatch.setattr(admission, "gecko_codes", lambda raw: codes)
    monkeypatch.setattr(admission, "stadium_is_frozen",
                        lambda start, codes: frozen)


def test_stadium_patch_set_decides_the_transformations(monkeypatch):
    start = {"stage": 3}
    _codes(monkeypatch, {}, frozen=True)
    assert rd.stadium_transformations(start, b"") == rd.STADIUM_FROZEN
    # Console and pre-3.18 Slippi: Common/Preload Stadium Transformations.
    _codes(monkeypatch, {0xC21D45EC: b""})
    assert rd.stadium_transformations(start, b"") == rd.STADIUM_PRELOADED
    # Slippi Online 3.18+: the synchronous StadiumFileLoad.
    _codes(monkeypatch, {0xC20165AC: b""})
    assert rd.stadium_transformations(start, b"") == rd.STADIUM_ONLINE_LOADED
    # The online toggle's static byte set: frozen after all.
    toggle = bytes.fromhex("c21d457c000000074800000c4e80002101000000")
    _codes(monkeypatch, {0xC20165AC: b"", 0xC21D457C: toggle})
    assert rd.stadium_transformations(start, b"") == rd.STADIUM_FROZEN
    # Neither patch set: the archive was streamed from the disc.
    _codes(monkeypatch, {})
    assert rd.stadium_transformations(start, b"") is None
    assert rd.supported({"players": [{}, {}], "stage": 3,
                         "stadium": None}) == \
        "stadium-unpatched-transformations"
    # Other stages never transform.
    assert rd.stadium_transformations({"stage": 31}, b"") == rd.STADIUM_FROZEN


def test_recording_that_does_not_start_at_zero_percent_is_not_driven():
    # Every match starts each fighter at 0%; a first row with damage on it
    # did not begin at the match's start.
    base = {"stage": 31, "stadium": 0}
    fresh = [{"percent": np.array([0.0, 3.0])}, {"percent": np.array([0.0])}]
    assert rd.supported(dict(base, players=fresh)) is None
    late = [{"percent": np.array([27.0])}, {"percent": np.array([78.0])}]
    assert rd.supported(dict(base, players=late)) == "not-from-match-start"


def _row(**lanes):
    """One OutFrame row, zero but for the named lanes."""
    values = list(struct.unpack(rd.OUT_FMT, bytes(rd.OUT_SIZE)))
    offsets = {"pos_x": 2, "pos_y": 6, "percent": 10, "shield_hp": 14,
               "action_id": 18, "stocks": 34, "facing": 38,
               "follower_present": 66, "follower_facing": 70,
               "follower_stocks": 74, "item_count": 78,
               "stadium_phase": 79, "stadium_kind": 80,
               "follower_action_id": 81, "follower_pos_x": 85,
               "follower_pos_y": 89, "follower_percent": 93,
               "follower_shield_hp": 97}
    items = lanes.pop("items", [])
    for name, value in lanes.items():
        base = offsets[name]
        if isinstance(value, (list, tuple)):
            values[base:base + len(value)] = value
        else:
            values[base] = value
    for slot, item in enumerate(items):
        base = rd.ITEM_BASE + slot * len(rd.ITEM_NAMES)
        for key, value in item.items():
            values[base + rd.ITEM_NAMES.index(key)] = value
    return struct.pack(rd.OUT_FMT, *values)


def _replay(**extra):
    player = {name: np.zeros(1) for name in
              ("post_x", "post_y", "percent", "shield", "state",
               "stocks_now")}
    player["direction"] = np.full(1, -1.0)
    player["valid"] = np.ones(1, dtype=bool)
    replay = {"players": [player], "items": None, "version": (3, 16, 0)}
    replay.update(extra)
    return replay


def test_nana_is_compared_while_the_recording_has_her():
    replay = _replay()
    player = replay["players"][0]
    player.update({"f_valid": np.ones(1, dtype=bool), "f_x": np.full(1, 5.0),
                   "f_y": np.zeros(1), "f_percent": np.zeros(1),
                   "f_shield": np.zeros(1), "f_state": np.zeros(1),
                   "f_stocks": np.zeros(1), "f_direction": np.full(1, -1.0)})
    same = _row(follower_present=[1, 0, 0, 0], follower_pos_x=[5.0, 0, 0, 0])
    assert rd.frame_diff(replay, same, 0) == []
    moved = _row(follower_present=[1, 0, 0, 0], follower_pos_x=[5.5, 0, 0, 0])
    assert rd.frame_diff(replay, moved, 0) == [
        ("follower.pos_x", 0, 5.5, 5.0)]
    asleep = _row()
    assert rd.frame_diff(replay, asleep, 0) == [
        ("follower.present", 0, 0.0, 1.0)]


def test_items_compare_their_gameplay_lanes():
    items = {name: np.zeros(1) for name in rd.ITEM_COMPARED}
    items["type"] = np.full(1, 99)                 # Peach's turnip
    items["misc"] = np.array([[7, 3, 9, 9]])
    items["instance_id"] = np.zeros(1)
    items["start"] = np.zeros(1, dtype=np.int64)
    items["count"] = np.ones(1, dtype=np.int64)
    replay = _replay(items=items)
    # The turnip's face (xDDB, misc1) is gameplay state; its other misc
    # bytes are not, so they may differ.
    turnip = {"exists": 1, "type": 99, "misc_mask": 0b0010,
              "misc0": 1, "misc1": 3, "misc2": 0, "misc3": 0}
    assert rd.frame_diff(replay, _row(item_count=1, items=[turnip]), 0) == []
    turnip["misc1"] = 4
    assert rd.frame_diff(replay, _row(item_count=1, items=[turnip]), 0) == [
        ("item.misc1", 0, 4.0, 3.0)]
    assert rd.frame_diff(replay, _row(), 0) == [("item.count", -1, 0.0, 1.0)]


def test_stadium_events_are_the_state_after_the_frame():
    replay = _replay(stadium_events=[None, (1, 9), (1, 9), (2, 9)])
    blob = _row(stadium_phase=1, stadium_kind=9) * 4
    assert rd.stadium_diff(replay, blob, 0) is None      # before any event
    assert rd.stadium_diff(replay, blob, 2) is None
    assert rd.stadium_diff(replay, blob, 3) == ((1, 9), (2, 9))


def test_ambiguous_cstick_gates_are_the_only_reconstruction_choice():
    player = {"c_x": np.array([0.0, -0.9875, -0.9875, -1.0, 0.5, 0.9875]),
              "c_y": np.array([0.0, 0.0, 0.0, 0.0, 0.0, 0.0125])}
    exact = rd.reconstructed_cstick(player, rd.GATE_EXACT)
    snap = rd.reconstructed_cstick(player, rd.GATE_SNAP)
    assert exact[0].tolist() == [0, -79, -79, -80, 40, 79]
    assert snap[0].tolist() == [0, -80, -80, -80, 40, 79]
    # One run per held gate position; the off-axis 0.0125 is not a gate.
    assert rd.cstick_gate_segments(player) == [(1, 2, -1)]


def test_neutral_spawn_is_read_from_the_code_list():
    from tools.validation import admission
    assert not admission.slippi_patches({0xC216E510: b""}) & admission.PATCH_RETAIL_SPAWNS
    assert admission.slippi_patches({0xC21D24FC: b""}) & admission.PATCH_RETAIL_SPAWNS
    assert admission.slippi_patches({}) == 0


def test_rollback_rows_and_a_discarded_tail():
    # Frames 0..4 written in order, then a rollback rewrites 2 and 3.
    ids = np.array([0, 1, 2, 3, 2, 3, 4])
    keep, dropped = rd.finalized_rows(ids)
    assert keep.tolist() == [0, 1, 4, 5, 6] and dropped == 0
    # The game ends mid re-simulation: 3 is rewritten before 4 ever was, so
    # 4's only row is from the discarded timeline.
    ids = np.array([0, 1, 2, 3, 4, 2, 3])
    keep, dropped = rd.finalized_rows(ids)
    assert keep.tolist() == [0, 1, 5, 6] and dropped == 1
    keep, dropped = rd.finalized_rows(np.array([], dtype=np.int32))
    assert len(keep) == 0 and dropped == 0


def test_human_level_byte_reaches_the_tape():
    import struct
    # A minimal raw array: the payload-sizes event, then Game Start.
    start = bytearray(1 + 0x74 + 0x24 * 3 + 1)
    start[0] = 0x36
    for port, level in enumerate((1, 0, 3, 9)):
        start[0x74 + 0x24 * port] = level
    sizes = bytes([0x35, 4, 0x36]) + struct.pack(">H", len(start) - 1)
    body = sizes + bytes(start)
    raw = b"{U\x03raw[$U#l" + struct.pack(">I", len(body)) + body
    assert rd.raw_cpu_levels(raw) == [1, 0, 3, 9]
    assert rd.raw_cpu_levels(b"not a replay") is None
    assert rd.tape_level({"type": "Human", "cpu_level": 0, "human_level": 1}) == 0x81
    assert rd.tape_level({"type": "Human", "cpu_level": 0, "human_level": 0}) == 0
    assert rd.tape_level({"type": "Cpu", "cpu_level": 7, "human_level": 0}) == 7


# A public tournament recording (Purdue Weeklies, 6 March 2020), not committed:
# found by file name under MSL_EXTERNAL_REPLAYS.
UCF_074_SLIPPI_3_3 = "Game_A45C27B4D631_20200306T215248.slpz"


def test_ucf_routines_a_slippi_3_3_list_lacks_stay_vanilla():
    """The 3.3 console recording behind the absent UCF routines (and the 3.3
    post-frame bit): UCF 0.74's set, no tumble fix. Exact through the end
    with the list's own profile and slippi_patches bits."""
    from pathlib import Path
    from tools.validation import admission
    from tools.validation.slpz import replay_path_for_peppi
    import os
    path = next((found for root in filter(None, os.environ.get(
                     "MSL_EXTERNAL_REPLAYS", "").split(os.pathsep))
                 for found in Path(root).rglob(UCF_074_SLIPPI_3_3)), None)
    if not rd.DRIVER.is_file():
        pytest.skip("replay_drive is not built")
    if not rd.DATA_ROOT.is_dir():
        pytest.skip("no extracted game data (%s)" % rd.DATA_ROOT)
    if path is None:
        pytest.skip("%s not found under MSL_EXTERNAL_REPLAYS" % UCF_074_SLIPPI_3_3)
    with replay_path_for_peppi(path) as slp:
        try:
            replay = rd.read(slp)
        except BaseException as error:  # peppi_py panics (a BaseException) on 3.3
            if isinstance(error, (KeyboardInterrupt, SystemExit)):
                raise
            pytest.skip("this peppi_py cannot read Slippi 3.3 (%s)" %
                        type(error).__name__)
    patches = replay["slippi_patches"]
    assert patches & admission.PATCH_POST_FRAME_AT_MAP
    assert patches & admission.PATCH_UCF_TUMBLE_ABSENT
    assert not patches & (admission.PATCH_UCF_DASHBACK_ABSENT |
                          admission.PATCH_UCF_SHIELD_DROP_ABSENT)
    assert replay["ucf_profile"].endswith("1")
    count = len(replay["ids"])
    blob, _ = rd.drive_arithmetic(replay, count, replay["ucf_profile"])
    first = rd.first_difference(replay, blob)
    assert first is None, rd.frame_diff(replay, blob, first)[:3]

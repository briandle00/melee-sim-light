"""The viewer track (tools/replay/replay_drive.export_viewer_track) on real
recordings, found by file name under MSL_EXTERNAL_REPLAYS and skipped when
they are missing, and its effect on the core's output (none)."""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path

import pytest

from tools.replay import replay_drive as rd

# A Final Destination Ice Climbers game (Nana, the background cycle) and a
# Battlefield game (the background switches).
RECORDINGS = [
    ("Game_20230311T193835.slp",
     "fdic"),
    ("Game_0019FD43AEAE_20240905T011354.slp",
     "bf"),
]

# Values after [frame, type]; "*" is any count.
ARITY = {"fx": 10, "fxend": 2, "gen": 6, "tint": (4, 9), "charge": 4, "part": 4,
         "pos": 9, "shake": 5, "hide": 3, "ganim": 5, "bg": 3, "fd": 5,
         "gcolor": 2, "bgcolor": 1, "cam": 9}


def _locate(name: str) -> Path | None:
    """A recording by file name under MSL_EXTERNAL_REPLAYS (os.pathsep-separated
    directories), None when it is not there."""
    for root in filter(None, os.environ.get("MSL_EXTERNAL_REPLAYS", "").split(os.pathsep)):
        for found in Path(root).rglob(name):
            if found.is_file():
                return found
    return None


def _prerequisites() -> str | None:
    if not rd.DRIVER.is_file():
        return "replay driver not built (%s)" % rd.DRIVER
    if not rd.DATA_ROOT.is_dir():
        return "no extracted game data (%s)" % rd.DATA_ROOT
    return None


def _check_event(event):
    assert isinstance(event, list) and len(event) >= 2
    frame, kind = event[0], event[1]
    assert isinstance(frame, int) and frame >= -123
    assert kind in rd.VIEWER_TRACK_TYPES
    arity = ARITY[kind]
    count = len(event) - 2
    if isinstance(arity, tuple):
        assert count in arity, event
    else:
        assert count == arity, event
    if kind in ("fx", "fxend"):
        owner_kind = event[3] if kind == "fx" else event[2]
        assert owner_kind in ("f", "i", "s", "-")


@pytest.mark.parametrize("path,name", RECORDINGS, ids=[n for _, n in RECORDINGS])
def test_viewer_track_schema(tmp_path, path, name):
    reason = _prerequisites()
    if reason:
        pytest.skip(reason)
    found = _locate(path)
    if found is None:
        pytest.skip("recording not found: %s" % path)
    path = found
    out = tmp_path / ("%s.json" % name)
    result = rd.export_viewer_track(path, out)
    data = json.loads(out.read_text())
    assert set(data) == {"version", "sim_build", "exact", "events"}
    assert data["version"] == rd.VIEWER_TRACK_VERSION
    assert isinstance(data["sim_build"], int)
    assert data["exact"] is True
    frames = [event[0] for event in data["events"]]
    assert frames == sorted(frames)
    for event in data["events"]:
        _check_event(event)
    kinds = {event[1] for event in data["events"]}
    assert {"fx", "tint", "cam", "part"} <= kinds
    if name == "fdic":
        assert "fd" in kinds
        # Nana's rows carry sub 1.
        assert any(e[1] == "tint" and e[3] == 1 for e in data["events"])
    if name == "bf":
        assert "bg" in kinds
    assert result["seconds"] > 0


def test_viewer_track_does_not_change_the_output(tmp_path):
    reason = _prerequisites()
    if reason:
        pytest.skip(reason)
    path = RECORDINGS[1][0]
    found = _locate(path)
    if found is None:
        pytest.skip("recording not found: %s" % path)
    path = found
    replay = rd.read(path)
    count = len(replay["ids"])
    profile = replay.get("ucf_profile") or rd.candidate_profiles(replay)[0]
    tape = tmp_path / "tape.bin"
    tape.write_bytes(rd.build_tape(replay, count, profile))
    digests = []
    for on in (False, True):
        env = dict(os.environ)
        env.pop("MSL_VIEWER_TRACK", None)
        if on:
            env["MSL_VIEWER_TRACK"] = str(tmp_path / "track.jsonl")
        out = tmp_path / ("out%d.bin" % on)
        subprocess.run([str(rd.DRIVER), str(rd.DATA_ROOT), str(tape), str(out)],
                       check=True, capture_output=True, env=env)
        digests.append(hashlib.sha256(out.read_bytes()).hexdigest())
    assert digests[0] == digests[1]
    assert (tmp_path / "track.jsonl").stat().st_size > 0

"""Recordings each fix made replay exactly, read from outside the repository.

No replay is committed. tests/fixtures/external_replays.json names each one
by its SHA-256 and the path it was found at. A recording is looked up at that
path, then by file name under MSL_EXTERNAL_REPLAYS (os.pathsep-separated
directories). A recording that is not found, a driver that is not built, or
game data that is not extracted skips the case.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

import pytest

from tools.replay import replay_drive as rd

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / "tests/fixtures/external_replays.json").read_text())


def _locate(entry) -> Path | None:
    candidates = [Path(entry["path"])]
    name = Path(entry["path"]).name
    for root in filter(None, os.environ.get("MSL_EXTERNAL_REPLAYS", "").split(os.pathsep)):
        candidates += list(Path(root).rglob(name))
    for path in candidates:
        if path.is_file() and \
                hashlib.sha256(path.read_bytes()).hexdigest() == entry["sha256"]:
            return path
    return None


def _prerequisites() -> str | None:
    if not rd.DRIVER.is_file():
        return "replay driver not built (%s)" % rd.DRIVER
    if not rd.DATA_ROOT.is_dir():
        return "no extracted game data (%s)" % rd.DATA_ROOT
    return None


@pytest.mark.parametrize(
    "entry", MANIFEST,
    ids=["%s-%s" % (e["fix"], e["sha256"][:8]) for e in MANIFEST])
def test_recording_replays_exactly(entry):
    reason = _prerequisites()
    if reason:
        pytest.skip(reason)
    path = _locate(entry)
    if path is None:
        pytest.skip("recording %s not found" % entry["sha256"][:12])
    replay = rd.read(path)
    assert rd.supported(replay) is None
    count = len(replay["ids"])
    own = replay.get("ucf_profile")
    first = None
    for profile in ([own] if own else rd.candidate_profiles(replay)):
        blob, _ = rd.drive_arithmetic(replay, count, profile)
        first = rd.first_difference(replay, blob)
        if first is None:
            break
    assert first is None, (
        "%s first differs at frame %d: %s" %
        (entry["fix"], int(replay["ids"][first]),
         rd.frame_diff(replay, blob, first)[:3]))

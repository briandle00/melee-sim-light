"""No recording is committed beyond upstream's LFS fixtures.

Upstream keeps its validation replays as Git LFS pointers under replays/.
Every tracked .slp or .slpz must be one of those: under replays/ and a
pointer (a few hundred bytes), never the recording itself.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _git(*args: str, stdin: str | None = None) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], input=stdin,
                          capture_output=True, text=True, check=True).stdout


def test_only_lfs_pointers_under_replays_are_tracked():
    try:
        listing = _git("ls-files", "-s", "--", "*.slp", "*.slpz", "*.SLP")
    except (OSError, subprocess.CalledProcessError):
        pytest.skip("not a git checkout")
    entries = [line.split(None, 3) for line in listing.splitlines() if line]
    outside = [path for _, _, _, path in entries if not path.startswith("replays/")]
    assert not outside, "recordings tracked outside replays/: %s" % outside[:5]
    if not entries:
        return
    sizes = _git("cat-file", "--batch-check=%(objectsize) %(objectname)",
                 stdin="".join(blob + "\n" for _, blob, _, _ in entries))
    large = [(path, int(size.split()[0]))
             for (_, _, _, path), size in zip(entries, sizes.splitlines())
             if int(size.split()[0]) > 1024]
    assert not large, "recordings committed in full, not as LFS pointers: %s" % large[:5]

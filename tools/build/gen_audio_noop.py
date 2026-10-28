"""Regenerates src/present/audio_noop.c (viewer build only: the sound and rumble calls the display pass makes, which do nothing) with
each function defined by the signature the decomp's header declares for it. A function defined with another signature than its callers use
links on a native 64-bit host but not in WebAssembly, where the linker makes a trapping thunk for the mismatch."""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "src/present/audio_noop.c"
names = re.findall(r"^msl_word (\w+)\(", OUT.read_text(), re.M) or re.findall(r"^[\w\s\*]+? (\w+)\(.*\) \{", OUT.read_text(), re.M)
headers = [ROOT / "src/melee/lb/lbaudio_ax.h", ROOT / "src/sysdolphin/baselib/rumble.h"]
text = "\n".join(h.read_text() for h in headers)
lines = []
for n in names:
    m = re.search(r"^[^\n(]*?\b([A-Za-z_][\w \*]*?)\s*\b" + n + r"\(([^)]*)\);", text, re.M)
    if not m:
        sys.exit(f"no prototype for {n}")
    ret = re.sub(r"/\*.*?\*/", "", m.group(0).split(n + "(")[0]).strip()
    params = [p.strip() for p in m.group(2).split(",")] if m.group(2).strip() not in ("", "void") else []
    TYPEWORDS = {"int", "char", "short", "long", "float", "double", "unsigned", "signed"}
    def bare(p):
        t = p.replace("*", " * ").split()
        if len(t) >= 2 and t[-1] not in TYPEWORDS and t[-1] != "*" and not (len(t) == 2 and t[0] in ("struct", "enum", "union", "const")):
            t = t[:-1]
        return " ".join(t).replace(" *", "*").replace("* ", "*") if "*" in p else " ".join(t)
    params = [bare(p) for p in params]
    plist = ", ".join(f"{p} a{i}" for i, p in enumerate(params)) or "void"
    voids = "".join(f" (void) a{i};" for i in range(len(params)))
    body = f"{voids} return 0;" if ret != "void" else voids
    lines.append(f"{ret} {n}({plist}) {{{body} }}")
head = '''/* Viewer build only (MSL_GX_RECORD): the game's sound and controller-rumble calls that the HUD, effects and stage code make while drawing.
 * The viewer plays no sound and has no pad; every call returns 0 at once. (The headless simulator reaches the same calls through its
 * own audio log; those are the Runtime/audio.c ones, not these.) Each is defined with the signature the decomp's header gives it
 * (tools/build/gen_audio_noop.py): a mismatch with the callers' signature traps in WebAssembly.
 * refs/melee/src/melee/lb/lbaudio_ax.c, refs/melee/src/sysdolphin/baselib/padrumble.c */

#if MSL_GX_RECORD

#include <melee/lb/lbaudio_ax.h>
#include <baselib/rumble.h>

'''
OUT.write_text(head + "\n".join(lines) + "\n\n#endif\n", newline="\n")
print(len(lines), "functions")

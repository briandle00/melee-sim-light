#!/usr/bin/env bash
# Windows build of the Python core library and the replay driver.
#
# The root Makefile cannot run here: there is no GNU make on a stock Windows
# box, no powerpc-linux-gnu cross toolchain, and the link line is ELF-only
# (-Wl,-z,defs, -Bsymbolic). This mirrors the `python-library` target's flags
# with mingw-w64, and adds the three things a PE target needs:
#
#   * tools/build/win_shim on the include path (sys/mman.h, dlfcn.h, link.h)
#   * devkitPPC in place of the Debian cross toolchain for the PPC DWARF types
#     object, which is only ever read for struct layouts and never linked
#   * named abort stubs for the unreachable upstream branches that the ELF
#     link drops with --gc-sections and the PE link cannot, because PE has no
#     symbol visibility and mingw auto-exports everything
#
# Writes only into the gitignored build/ tree. Run it from anywhere:
#
#   bash tools/build/build_windows.sh
#
# Environment:
#   CC        host compiler (default: gcc)
#   PPC_CC    PowerPC compiler for the DWARF types object
#             (default: devkitPPC's powerpc-eabi-gcc)
#   PY        Python for the code generators (default: .venv/Scripts/python.exe)
#   OPT       optimization level for the core (default: -O2; -O0 is the
#             gate profile and produced identical output here)
#   JOBS      parallel compiles (default: 16)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Paths inside a linker response file are never MSYS-converted, so the whole
# build works in Windows form.
case "$(uname -s)" in
  MINGW*|MSYS*) ROOT="$(cd "$ROOT" && pwd -W)" ;;
esac
CORE="$ROOT/src"
SHIM="$ROOT/tools/build/win_shim"
BUILD="$ROOT/build/melee_core"
GEN="$BUILD/native/generated"
OBJ="$BUILD/python/obj"

CC="${CC:-gcc}"
PPC_CC="${PPC_CC:-/c/devkitPro/devkitPPC/bin/powerpc-eabi-gcc}"
PY="${PY:-$ROOT/.venv/Scripts/python.exe}"
OPT="${OPT:--O2}"
JOBS="${JOBS:-16}"

CPPFLAGS=(
  -DBUGFIX
  -include "$CORE/platform/compat.h"
  -I"$CORE" -I"$CORE/platform/include" -I"$CORE/melee"
  -I"$CORE/melee/ft/chara" -I"$CORE/sysdolphin" -I"$CORE/Runtime"
  -I"$CORE/extern/dolphin/include"
  -I"$SHIM"
)
NATIVE_CPPFLAGS=("${CPPFLAGS[@]}" -DMSL_CORE_NATIVE -I"$GEN")
PYTHON_CPPFLAGS=("${NATIVE_CPPFLAGS[@]}" -DMSL_CORE_SHARED -D_GNU_SOURCE)
CFLAGS=("$OPT" -g -std=gnu11 -fgnu89-inline -fno-short-enums
        -fno-strict-aliasing -ffp-contract=off -ffunction-sections
        -fdata-sections -w
        -Werror=implicit-function-declaration -Wno-int-conversion
        -Wno-incompatible-pointer-types -fno-pie -fPIC -fvisibility=hidden)
# Melee's matching MSL trig compiles to fmadds/fnmsubs; the Makefile gives
# these two the same treatment.
FAST_CFLAGS=(-O2 -ffp-contract=fast -fno-builtin-sinf -fno-builtin-cosf -mfma)

mkdir -p "$GEN" "$OBJ" "$BUILD/python"
step() { printf '\n== %s\n' "$*"; }

step "PPC DWARF types"
"$PPC_CC" "${CPPFLAGS[@]}" -g -gdwarf-4 -fno-eliminate-unused-debug-types \
  -std=gnu11 -w -c "$ROOT/tools/build/native_dat_types.c" \
  -o "$GEN/native_dat_types_ppc.o"

step "host DWARF types"
"$CC" "${NATIVE_CPPFLAGS[@]}" -g -gdwarf-4 -w -fno-eliminate-unused-debug-types \
  -std=gnu11 -c "$ROOT/tools/build/native_dat_types.c" \
  -o "$GEN/native_dat_types_native.o"

step "generated command accessors and DAT layout"
"$PY" "$ROOT/tools/build/generate_command_accessors.py" \
  --object "$GEN/native_dat_types_ppc.o" --output "$GEN/msl_command_fields.h"
"$PY" "$ROOT/tools/build/generate_native_dat_layout.py" \
  --ppc "$GEN/native_dat_types_ppc.o" --native "$GEN/native_dat_types_native.o" \
  --output "$GEN/native_dat_layout.c"

step "generated match relocation layout"
"$CC" "${NATIVE_CPPFLAGS[@]}" -g -gdwarf-4 -w -fno-eliminate-unused-debug-types \
  -std=gnu11 -c "$ROOT/tools/build/match_reloc_types.c" \
  -o "$GEN/match_reloc_types.o"
"$PY" "$ROOT/tools/build/generate_match_reloc_layout.py" \
  --object "$GEN/match_reloc_types.o" \
  --types "$CORE/runtime/relocation_types.def" \
  --output "$GEN/match_reloc_layout.c"

step "enumerating sources"
mapfile -t SRCS < <(
  cd "$ROOT"
  git ls-files --cached --others --exclude-standard -- src |
    grep '\.c$' | grep -v '^src/runtime/main\.c$'
)
SRCS+=("build/melee_core/native/generated/native_dat_layout.c")
SRCS+=("build/melee_core/native/generated/match_reloc_layout.c")
echo "${#SRCS[@]} translation units"

step "compiling ($JOBS jobs, $OPT)"
printf '%s\n' "${SRCS[@]}" | xargs -P "$JOBS" -I{} bash -c '
  rel="{}"; out="'"$OBJ"'/${rel%.c}.o"; mkdir -p "$(dirname "$out")"
  extra=""
  case "$rel" in
    src/MSL/trigf.c|src/runtime/math.c)
      extra="'"${FAST_CFLAGS[*]}"'" ;;
  esac
  "'"$CC"'" '"${PYTHON_CPPFLAGS[*]}"' '"${CFLAGS[*]}"' \
    -I"'"$ROOT"'/$(dirname "$rel")" $extra \
    -c "'"$ROOT"'/$rel" -o "$out" 2>"$out.log" \
    || { echo "FAILED $rel"; head -20 "$out.log"; exit 1; }
' || { echo "compile step failed"; exit 1; }

step "linking libmelee_core.dll"
find "$OBJ" -name '*.o' | sort > "$BUILD/python/objects.txt"
link() {
  "$CC" -shared -Wl,--gc-sections -Wl,--exclude-all-symbols \
    "@$BUILD/python/objects.txt" "$ROOT/tools/build/melee_core.def" \
    ${1:+"$1"} -static-libgcc -lm -o "$BUILD/python/libmelee_core.dll" 2>&1
}
missing="$BUILD/python/missing.txt"
: > "$missing"
for attempt in 1 2 3 4 5 6; do
  stub_obj=""
  if [ -s "$missing" ]; then
    "$PY" "$ROOT/tools/build/generate_win_stubs.py" "$missing" \
      "$BUILD/python/win_stubs.c" > /dev/null
    "$CC" -O0 -w -c "$BUILD/python/win_stubs.c" -o "$BUILD/python/win_stubs.o"
    stub_obj="$BUILD/python/win_stubs.o"
  fi
  if link "$stub_obj" > "$BUILD/python/link.log" 2>&1; then
    echo "linked with $(wc -l < "$missing" | tr -d ' ') stubbed branches"
    break
  fi
  grep -o "undefined reference to \`[A-Za-z0-9_]*'" "$BUILD/python/link.log" |
    sed "s/.*\`//; s/'//" >> "$missing"
  sort -u "$missing" -o "$missing"
done
[ -f "$BUILD/python/libmelee_core.dll" ] || { cat "$BUILD/python/link.log"; exit 1; }

step "replay driver"
"$CC" "${NATIVE_CPPFLAGS[@]}" -O2 -g -std=gnu11 -w \
  -c "$ROOT/tools/replay/replay_drive.c" -o "$BUILD/python/replay_drive.o"
"$CC" -g -Wl,--gc-sections "$BUILD/python/replay_drive.o" \
  "@$BUILD/python/objects.txt" "$BUILD/python/win_stubs.o" \
  -static-libgcc -lm -o "$BUILD/python/replay_drive.exe"

# ctypes needs the mingw runtime beside the library.
for dll in libwinpthread-1.dll; do
  found="$(dirname "$(command -v "$CC")")/$dll"
  [ -f "$found" ] && cp -f "$found" "$BUILD/python/" || true
done

ls -la "$BUILD/python/libmelee_core.dll" "$BUILD/python/replay_drive.exe"

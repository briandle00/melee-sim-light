#!/usr/bin/env bash
# The viewer build (MSL_GX_RECORD=1) as WebAssembly, for the browser: the same sources as tools/build/build_windows.sh, compiled with
# emscripten the way the Makefile's `wasm` target does (-DMSL_CORE_NATIVE -DMSL_CORE_WASM, struct layouts from a Linux i386 proxy object
# with -malign-double), plus the GX layer (src/gx). Two outputs in build/wasm_gx:
#   gx-core.{js,wasm}      the module for the page (src/gx/gx_wasm.c's per-frame API; the game data is added with --preload-file when DATA is set)
#   replay_drive.{js,wasm} the replay driver for node (NODERAWFS), to check the wasm stream against the native one
# Environment: EMSDK (default $HOME/emsdk), PY (the generators' Python), JOBS, OPT (default -O2), DATA (a raw game-data dir to package into gx-core).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "$(uname -s)" in MINGW*|MSYS*) ROOT="$(cd "$ROOT" && pwd -W)" ;; esac
EMSDK="${EMSDK:-$HOME/emsdk}"
export PATH="$(cygpath -u "$EMSDK" 2>/dev/null || echo "$EMSDK"):$(cygpath -u "$EMSDK/upstream/emscripten" 2>/dev/null || echo "$EMSDK/upstream/emscripten"):$(cygpath -u "$EMSDK/node/24.19.0_64bit/bin" 2>/dev/null):$PATH"
CORE="$ROOT/src"
BUILD="$ROOT/build/wasm_gx"
GEN="$BUILD/generated"
OBJ="$BUILD/obj"
PY="${PY:-python3}"
OPT="${OPT:--O2}"
JOBS="${JOBS:-16}"
CLANG="$EMSDK/upstream/bin/clang.exe"
LAYOUT_INC="$GEN/layout-include"
mkdir -p "$GEN" "$OBJ" "$LAYOUT_INC/gnu"
: > "$LAYOUT_INC/gnu/stubs-32.h"
step() { printf '\n== %s\n' "$*"; }

CPPFLAGS=(-DBUGFIX -include "$CORE/platform/compat.h" -I"$CORE" -I"$CORE/platform/include" -I"$CORE/melee" -I"$CORE/melee/ft/chara"
  -I"$CORE/sysdolphin" -I"$CORE/Runtime" -I"$CORE/extern/dolphin/include" -I"$CORE/present/include" -DMSL_GX_RECORD=1 -I"$CORE/gx/sdk"
  -I"$GEN" -DMSL_CORE_NATIVE -DMSL_CORE_WASM)
# (-mnontrapping-fptoint: a float converted to an integer out of range saturates, as the PPC's fctiwz does, instead of trapping)
CFLAGS=("$OPT" -g0 -mnontrapping-fptoint -std=gnu11 -fgnu89-inline -fno-short-enums -fwrapv -fno-strict-aliasing -ffp-contract=off -ffunction-sections -fdata-sections -w
  -Werror=implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types
  -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-tanf -fno-builtin-sincosf -fno-builtin-atan2f -fno-builtin-atanf -fno-builtin-acosf -fno-builtin-asinf ${EXTRA_CFLAGS:-})
PROXY=("$CLANG" --target=i386-unknown-linux-gnu -malign-double -nostdlibinc -isystem "$ROOT/tools/build/cross_libc_shim" -I"$LAYOUT_INC")

step "layout proxy objects (i386)"
"${PROXY[@]}" "${CPPFLAGS[@]}" -g -gdwarf-4 -w -fno-eliminate-unused-debug-types -std=gnu11 -c "$ROOT/tools/build/native_dat_types.c" -o "$GEN/native_dat_types_wasm.o"
"${PROXY[@]}" "${CPPFLAGS[@]}" -g -gdwarf-4 -w -fno-eliminate-unused-debug-types -std=gnu11 -c "$ROOT/tools/build/match_reloc_types.c" -o "$GEN/match_reloc_types.o"

step "generated sources"
"$PY" "$ROOT/tools/build/generate_command_accessors.py" --ppc "$ROOT/tools/build/ppc_layout.json" --output "$GEN/msl_command_fields.h"
"$PY" "$ROOT/tools/build/generate_native_dat_layout.py" --draw-objects --ppc "$ROOT/tools/build/ppc_layout_gx.json" --native "$GEN/native_dat_types_wasm.o" --output "$GEN/native_dat_layout.c"
"$PY" "$ROOT/tools/build/generate_match_reloc_layout.py" --object "$GEN/match_reloc_types.o" --types "$CORE/runtime/relocation_types.def" --output "$GEN/match_reloc_layout.c"

step "sources"
mapfile -t SRCS < <(cd "$ROOT"; git ls-files --cached --others --exclude-standard -- src | grep '\.c$' | grep -v '^src/runtime/main\.c$' | grep -v '^src/python_api\.c$')
SRCS+=("build/wasm_gx/generated/native_dat_layout.c" "build/wasm_gx/generated/match_reloc_layout.c")
echo "${#SRCS[@]} translation units"

# The same float rules as the native viewer build (tools/build/float_rules.json): fused multiply-adds as explicit llvm.fma (a call of fmaf here: exact), the
# game's inline Gekko square root. MSL_VIEWER_LLVM=0 compiles with plain emcc instead (no fusing at all; the streams then differ from native).
CCOBJ="emcc"
if [ "${MSL_VIEWER_LLVM:-1}" = 1 ]; then CCOBJ="$PY $ROOT/tools/build/llvmcc.py"; export MSL_LLVM_TARGET=wasm; fi
export CCOBJ
# The objects are rebuilt when the float rules or the compile driver change (the per-file check below only sees a newer source file: after a rules change a
# wasm object kept the old, unfused arithmetic while the native one was rebuilt, and the two streams differed; the native build has the same stamp, LLVMSUM).
RULESUM="viewer_llvm=${MSL_VIEWER_LLVM:-1} $(cat "${MSL_FLOAT_RULES:-$ROOT/tools/build/float_rules.json}" "$ROOT/tools/build/llvmcc.py" "$ROOT/tools/build/floatxform.py" | md5sum | cut -c1-12)"
if [ "$(cat "$OBJ/.rulestamp" 2>/dev/null)" != "$RULESUM" ]; then FORCE=1; export FORCE; fi
step "compiling ($JOBS jobs, $OPT)"
export EMCC_SKIP_SANITY_CHECK=1
printf '%s\n' "${SRCS[@]}" | xargs -P "$JOBS" -I{} bash -c '
  rel="{}"; out="'"$OBJ"'/${rel%.c}.o"; mkdir -p "$(dirname "$out")"
  if [ -f "$out" ] && [ "$out" -nt "'"$ROOT"'/$rel" ] && [ -z "'"${FORCE:-}"'" ]; then exit 0; fi
  extra=""; first=""; file="'"$ROOT"'/$rel"
  case "$rel" in
    src/MSL/trigf.c|src/runtime/math.c) extra="-O2 -ffp-contract=fast -fno-builtin-sinf -fno-builtin-cosf" ;;
    src/present/*.c)
      first="$(ROOT="'"$ROOT"'" bash "'"$ROOT"'/tools/build/present_flags.sh" "$rel" first)"
      extra="$(ROOT="'"$ROOT"'" bash "'"$ROOT"'/tools/build/present_flags.sh" "$rel")"
      # (emscripten'"'"'s clang has no iconv: the unit whose strings are Shift-JIS (-fexec-charset=CP932) is converted first, its string literals
      #  written as octal escapes of their CP932 bytes: tools/build/cp932_src.py; the names the HUD shows come from them)
      case "$extra" in *-fexec-charset=CP932*)
        cp="'"$GEN"'/cp932"; mkdir -p "$cp/decomp/melee/gm"
        "'"$PY"'" "'"$ROOT"'/tools/build/cp932_src.py" "'"$ROOT"'/src/present/decomp/melee/gm/gm_1601.c.inc" "$cp/decomp/melee/gm/gm_1601.c.inc" || exit 1
        cp "'"$ROOT"'/$rel" "$cp/$(basename "$rel")"; file="$cp/$(basename "$rel")"; extra="$extra -I'"$ROOT"'/src/present/decomp/melee/gm" ;;
      esac
      extra="${extra//-fexec-charset=CP932/}"
      extra="${extra//-finput-charset=UTF-8/}"
      # (x86 instruction-set flags: the units own #if guards fall back to the portable code; wasm has no -mfma)
      extra="$(printf %s "$extra" | sed -E "s/-m(fma|sse[0-9._]*|ssse3|avx[0-9a-z]*)( |\$)//g")" ;;
  esac
  '"$CCOBJ"' $first '"${CPPFLAGS[*]}"' '"${CFLAGS[*]}"' -I"'"$ROOT"'/$(dirname "$rel")" $extra -c "$file" -o "$out" 2>"$out.log" \
    || { echo "FAILED $rel"; head -20 "$out.log"; exit 1; }
' || { echo "compile step failed"; exit 1; }
printf %s "$RULESUM" > "$OBJ/.rulestamp"

step "driver objects"
emcc "${CPPFLAGS[@]}" -O2 -std=gnu11 -w -c "$ROOT/tools/replay/replay_drive.c" -o "$OBJ/replay_drive.o"
for rel in "${SRCS[@]}"; do echo "$OBJ/${rel%.c}.o"; done | sort > "$BUILD/objects.txt"
# Named abort stubs for the unreachable upstream branches (what build_windows.sh does): the names a link left undefined, kept in
# tools/build/wasm_missing.txt (the names a link without them left undefined: grep "undefined symbol" in $BUILD/link.log). Without a stub a call to one traps as "null function" with no name.
if [ -s "$ROOT/tools/build/wasm_missing.txt" ]; then
  "$PY" "$ROOT/tools/build/generate_win_stubs.py" "$ROOT/tools/build/wasm_missing.txt" "$BUILD/win_stubs.c" > /dev/null
  emcc -O0 -w -c "$BUILD/win_stubs.c" -o "$OBJ/win_stubs.o"
  echo "$OBJ/win_stubs.o" >> "$BUILD/objects.txt"
fi
echo "$(wc -l < "$BUILD/objects.txt") objects"

# The decomp registers callbacks through casts to another arity (a proc written as void f(void) called with its GObj, a render function called as a proc): fine on
# the PPC and on a 64-bit host, a trap in WebAssembly. -sEMULATE_FUNCTION_POINTER_CASTS gives every table function an adapter that takes any arguments.
COMMON=(${FPCAST-"-sEMULATE_FUNCTION_POINTER_CASTS=1"} -Wl,--gc-sections -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=536870912 -sMAXIMUM_MEMORY=2147483648 -sSTACK_SIZE=16777216 -lm)
# The unreachable upstream branches name symbols the port does not have (build_windows.sh stubs them by name); emscripten turns a missing function
# into one that aborts when called and a missing datum into address 0.
link() {  # link <output.js> <extra emcc args...>
  local out="$1"; shift
  emcc "@$BUILD/objects.txt" "${COMMON[@]}" -sERROR_ON_UNDEFINED_SYMBOLS=0 -Wl,--unresolved-symbols=ignore-all ${EXTRA_LINK:-} "$@" -o "$out" 2>"$BUILD/link.log" || { grep -v "^>>>\|signature mismatch\|^$" "$BUILD/link.log" | head -40; return 1; }
}

step "replay_drive for node"
link "$BUILD/replay_drive.js" ${NAMES:+--profiling-funcs} "$OBJ/replay_drive.o" -O2 -sNODERAWFS=1 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sSTACK_SIZE=16777216
ls -la "$BUILD"/replay_drive.js "$BUILD"/replay_drive.wasm

# the module for the page: the per-frame API of tools/replay/gx_wasm_api.c
step "gx-core (the page's module)"
emcc "${CPPFLAGS[@]}" -O2 -std=gnu11 -w -I"$ROOT/tools/replay" -c "$ROOT/tools/replay/gx_wasm_api.c" -o "$OBJ/gx_wasm_api.o"
EXPORTS='["_malloc","_free","_msl_gxw_open","_msl_gxw_set_playback","_msl_gxw_set_display","_msl_gxw_restart_stream","_msl_gxw_heap_top","_msl_gxw_playback","_msl_gxw_set_hud_layout","_msl_gxw_hud_layout","_msl_gxw_cfit_new","_msl_gxw_cfit_lane","_msl_gxw_cfit_raw","_msl_gxw_cfit_next","_msl_gxw_cfit_result","_msl_gxw_cfit_first","_msl_gxw_cfit_flips","_msl_gxw_cfit_mode","_msl_gxw_set_recorded","_msl_gxw_step","_msl_gxw_step_frame","_msl_gxw_tape_frame_size","_msl_gxw_frame_index","_msl_gxw_frame_count","_msl_gxw_stream_ptr","_msl_gxw_stream_len","_msl_gxw_region_count","_msl_gxw_region_addr","_msl_gxw_region_ptr","_msl_gxw_region_size","_msl_gxw_region_type","_msl_gxw_region_pos","_msl_gxw_initial_state","_msl_gxw_output_ptr","_msl_gxw_output_size","_msl_gxw_out_row","_msl_gxw_out_row_size"]'
DATA="${DATA:-}"
PRELOAD=()
if [ -n "$DATA" ]; then PRELOAD=(--preload-file "$DATA@/data/raw"); fi
link "$BUILD/gx-core.js" "$OBJ/gx_wasm_api.o" -O2 --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createGxCore -sENVIRONMENT=web,node \
  -sEXPORTED_FUNCTIONS="$EXPORTS" -sEXPORTED_RUNTIME_METHODS='["HEAPU8","FS"]' -sFORCE_FILESYSTEM=1 ${NAMES:+--profiling-funcs} "${PRELOAD[@]}"
ls -la "$BUILD"/gx-core.*

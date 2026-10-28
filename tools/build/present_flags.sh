#!/usr/bin/env bash
# The extra include directories a src/present wrapper asks for (viewer build only; the wrappers are empty translation units
# without MSL_GX_RECORD):
#   // present-include: <dir under src>   searched after the build's own -I directories (the default)
#   // present-first: <dir under src>     searched before them (a header that replaces the simulator's for this unit)
#   // present-flags: <compiler flags>    added to the unit's compile (the decomp's Shift-JIS strings: -fexec-charset=CP932)
# usage: present_flags.sh <src/present/x.c> [first]
rel=$1
mode=${2:-last}
root=${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
case "$rel" in
  src/present/*.c)
    if [ "$mode" = first ]; then key=present-first; else key=present-include; fi
    sed -n 's|^// '"$key"': \(.*\)$|-I'"$root"'/src/\1|p' "$root/$rel" | tr -d '\r'
    if [ "$mode" != first ]; then sed -n 's|^// present-flags: \(.*\)$|\1|p' "$root/$rel" | tr -d '\r'; fi ;;
esac

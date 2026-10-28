#!/usr/bin/env bash
# wasm_native_gate.sh <tree> <data root> <tape>...: per tape, the native viewer driver's per-frame stream hash (MSL_GX_RECORD_OUT=hash) against the
# wasm driver's (node) and the two gameplay outputs; prints IDENTICAL or the first frame that differs. Exit 1 if any differs.
# (the integrator's gate run: a wasm build that is not byte-equal to the native one fails here)
T=$1; D=$2; shift 2
NODE=${NODE:-/d/src/emsdk/node/24.19.0_64bit/node.exe}
W=${TMPDIR:-/tmp}/wng.$$; mkdir -p $W; rc=0
for TAPE in "$@"; do
  n=$(basename $TAPE .tape)
  MSL_GX_RECORD_OUT=hash MSL_GX_RECORD_FRAMES=0:99999999 $T/build/melee_core/python/replay_drive.exe $D $TAPE $W/n.out 2>&1 | grep GXHASH > $W/n.txt
  MSL_GX_RECORD_OUT=hash MSL_GX_RECORD_FRAMES=0:99999999 $NODE --stack-size=8000 $T/build/wasm_gx/replay_drive.js $D $TAPE $W/w.out 2>&1 | grep GXHASH > $W/w.txt
  first=$(paste $W/n.txt $W/w.txt | awk '$3!=$6{print NR-1; exit}')
  if cmp -s $W/n.txt $W/w.txt && [ -s $W/n.txt ] && cmp -s $W/n.out $W/w.out; then echo "$n frames $(wc -l < $W/n.txt) IDENTICAL"
  else echo "$n frames native $(wc -l < $W/n.txt) wasm $(wc -l < $W/w.txt) DIFFERENT (first frame ${first:-none}; gameplay output $(cmp -s $W/n.out $W/w.out && echo same || echo DIFFERENT))"; rc=1; fi
done
rm -rf $W; exit $rc

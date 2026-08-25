#!/usr/bin/env bash
# THE CAMPAIGN LADDER, built out of ONE compiler binary.
#
#   bash bench/tools/ladder.sh <build-dir-bronze-exe> <output-dir>
#
# Every stage of the E campaign left a seam behind, and every seam is read once
# per invocation and leaves the rest of the compiler alone. So peeling them in
# campaign order reproduces each stage's shipped compiler from the compiler
# that ships today — which is the only way to put five stages in one table
# without the cross-session drift that stages E2 and E3 both documented (the
# millisecond fixtures move ±50 % between sessions on identical IL).
#
# The manifest is the COMMITTED one in every row. Peeling seams is a statement
# about the compiler; changing the manifest under it would be a statement about
# the manifest, and the two must not be read out of one table.
set -e
B=${1:?bronze exe}
OUT=${2:-.}
mkdir -p "$OUT"
HERE=$(cd "$(dirname "$0")/.." && pwd)

# Every seam this campaign added, all on: stage 3.4's compiler.
S4="BRONZE_NO_PURE_PREDICATES=1 BRONZE_NO_CLOSURE_PARAM_PROOF=1 BRONZE_NO_DEFINITE_REACH=1"
S3="BRONZE_NO_FRAME_MERGE=1 BRONZE_NO_DEFINITE_INIT=1 $S4"
S2="BRONZE_NO_INLINE_TOINT32=1 BRONZE_NO_PURE_CONVERSIONS=1 BRONZE_NO_ENV_TRIPWIRE=1 $S3"
S1="BRONZE_NO_CLOSURE_EDGE=1 $S2"
# Iterated by TAG, never as one whitespace-joined list: each column's seam set
# is itself whitespace-separated, so a `for col in $COLS` over `tag:seams`
# pairs splits every seam into a column of its own and silently builds a table
# that is not the ladder. It looks like it worked — 85 executables instead of
# 55 is the only tell.
TAGS="s34 e1 e2 e3 e4"
seams_for() {
  case $1 in
    s34) echo "$S1" ;;
    e1) echo "$S2" ;;
    e2) echo "$S3" ;;
    e3) echo "$S4" ;;
    e4) echo "" ;;
  esac
}

# The small twins the two-count deltas need. `env_slot_kernel` and
# `nullish_pin_kernel` have no committed twin; the second count is the same
# file with ITERS divided by ten.
sed 's/const ITERS = 6000000;/const ITERS = 600000;/' "$HERE/env_slot_kernel.js" > "$OUT/es_small.js"
sed 's/8000000/800000/' "$HERE/nullish_pin_kernel.js" > "$OUT/nl_small.js"

build() { # tag envs src out pins
  local envs=$1 src=$2 out=$3 pins=$4
  if [ -n "$pins" ]; then
    env $envs "$B" build "$src" -o "$OUT/$out" --pins "$pins" >/dev/null 2>&1
  else
    env $envs "$B" build "$src" -o "$OUT/$out" >/dev/null 2>&1
  fi
}

for tag in $TAGS; do
  envs=$(seams_for "$tag")
  build "$envs" "$HERE/env_slot_kernel.js"   "es_${tag}_b.exe" "$HERE/pins/env-slot-kernel.pins"
  build "$envs" "$OUT/es_small.js"           "es_${tag}_s.exe" "$HERE/pins/env-slot-kernel.pins"
  build "$envs" "$HERE/mat4_kernel.js"       "m4_${tag}_b.exe" "$HERE/pins/threejs-math.pins"
  build "$envs" "$HERE/mat4_kernel_small.js" "m4_${tag}_s.exe" "$HERE/pins/threejs-math.pins"
  build "$envs" "$HERE/call_chain_kernel.js" "cc_${tag}.exe"   "$HERE/pins/call-chain-kernel.pins"
  build "$envs" "$HERE/nullish_pin_kernel.js" "nl_${tag}_b.exe" "$HERE/pins/nullish-kernel.pins"
  build "$envs" "$OUT/nl_small.js"           "nl_${tag}_s.exe" "$HERE/pins/nullish-kernel.pins"
  for f in typed_array_crunch three_math mesh_churn_2k object_graph; do
    build "$envs" "$HERE/$f.js" "${f}_${tag}.exe" ""
  done
  echo "built $tag"
done
echo DONE

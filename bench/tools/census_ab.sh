#!/usr/bin/env bash
# THE CENSUS ROUND TRIP, as an A/B (stage C1, src/runtime/pin_census.h).
#
#   bash bench/tools/census_ab.sh <bronze.exe> <output-dir>
#
# The question this harness exists to answer is not "is the census fast" — a
# census build is an instrument and is never benchmarked. It is: **does the
# manifest the census WROTE buy what the manifest a person wrote bought?**
#
# So every fixture gets two builds out of ONE compiler binary, differing only
# in which manifest they were handed:
#
#   hand   the committed bench/pins/*.pins, hand-authored against the source
#   inf    the file this script's own census run produced, minutes earlier
#
# and a third where the distinction has teeth:
#
#   infobs the same census file with its `@observed` entries ACCEPTED
#          (--pins-allow-observed). A default build refuses those — some store
#          to a field of that name is through a receiver the compiler cannot
#          type, so a violation there would be silent — and this column is what
#          says whether refusing them costs anything.
#
# `three_math` has no committed manifest, so its columns are `none` and `inf`:
# the first evidence of what inference is worth on three.js-shaped code that
# nobody has hand-written a manifest for.
#
# The protocol is bench/tools/interleave.py's and is not negotiable: one run of
# every column per round, medians, warmup discarded, 101 rounds for a kernel and
# 51 for a millisecond fixture. Every fixture times its own compute region
# (bench/harness.js), so no column here carries a process startup and no column
# needs a small twin to cancel one.
set -e
B=${1:?bronze exe}
OUT=${2:-.}
mkdir -p "$OUT"
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(cd "$OUT" && pwd)

# --- 1. THE CENSUS RUNS ------------------------------------------------------
# One instrumented build per fixture, one run each, one manifest each.
census() { # src outname
  "$B" build "$1" -o "$OUT/$2_census.exe" --census "$OUT/$2.pins" >/dev/null 2>&1
  (cd "$OUT" && "./$2_census.exe" >/dev/null 2>&1)
  # The DEFAULT-SAFE subset: what a build with no `--pins-allow-observed`
  # accepts. `@observed` also appears in the file's header prose, which is a
  # comment either way.
  grep -v '@observed' "$OUT/$2.pins" > "$OUT/$2_safe.pins"
  printf '%-16s %3d entries, %3d marked @observed\n' "$2" \
    "$(grep -vc '^#\|^$' "$OUT/$2.pins" || true)" \
    "$(grep -v '^#' "$OUT/$2.pins" | grep -c '@observed' || true)"
}
census "$HERE/env_slot_kernel.js"   es
census "$HERE/mat4_kernel.js"       m4
census "$HERE/nullish_pin_kernel.js" nl
census "$HERE/call_chain_kernel.js" cc
census "$HERE/three_math.js"        tm
# The vendored library end to end. Not a benchmark and not timed — it is an
# ORACLE program, so it prints a committed expectation rather than a checksum
# and a `[bench]` line, and the check it earns its place with is the `cmp`
# below. What it is good for is the manifest: 101 entries over 1718 candidate
# sites, 20 of them `@observed`.
census "$HERE/../tests/oracle/threejs/main.js" tj

build() { # src out pins extra
  if [ -n "$3" ]; then
    "$B" build "$1" -o "$OUT/$2" --pins "$3" $4 >/dev/null 2>&1
  else
    "$B" build "$1" -o "$OUT/$2" >/dev/null 2>&1
  fi
}

# --- 2. THE COLUMNS ----------------------------------------------------------
build "$HERE/env_slot_kernel.js" "es_hand.exe" "$HERE/pins/env-slot-kernel.pins"
build "$HERE/env_slot_kernel.js" "es_inf.exe"  "$OUT/es_safe.pins"

build "$HERE/mat4_kernel.js"       "m4_hand.exe" "$HERE/pins/threejs-math.pins"
build "$HERE/mat4_kernel.js"       "m4_inf.exe"  "$OUT/m4_safe.pins"
build "$HERE/mat4_kernel.js"       "m4_infobs.exe" "$OUT/m4.pins" --pins-allow-observed

build "$HERE/nullish_pin_kernel.js" "nl_hand.exe" "$HERE/pins/nullish-kernel.pins"
build "$HERE/nullish_pin_kernel.js" "nl_inf.exe"  "$OUT/nl_safe.pins"

build "$HERE/call_chain_kernel.js" "cc_hand.exe" "$HERE/pins/call-chain-kernel.pins"
build "$HERE/call_chain_kernel.js" "cc_inf.exe"  "$OUT/cc_safe.pins"

build "$HERE/three_math.js" "tm_none.exe" ""
build "$HERE/three_math.js" "tm_hand.exe" "$HERE/pins/threejs-math.pins"
build "$HERE/three_math.js" "tm_inf.exe"  "$OUT/tm_safe.pins"
build "$HERE/three_math.js" "tm_infobs.exe" "$OUT/tm.pins" --pins-allow-observed

TJ="$HERE/../tests/oracle/threejs/main.js"
build "$TJ" "tj_none.exe" ""
build "$TJ" "tj_inf.exe"  "$OUT/tj_safe.pins"
build "$TJ" "tj_infobs.exe" "$OUT/tj.pins" --pins-allow-observed
# The oracle's own expectation, checked here rather than trusted: a manifest
# that changes what three.js PRINTS is a miscompile, and this is the one
# fixture in the set whose correct output is committed.
for t in none inf infobs; do
  (cd "$OUT" && "./tj_$t.exe" > "tj_$t.out" 2>&1)
  if ! cmp -s "$OUT/tj_$t.out" "$HERE/../tests/oracle/threejs/main.expected"; then
    echo "MISCOMPILE: tj_$t.exe output differs from main.expected" >&2
    exit 1
  fi
done
echo "tj: byte-identical to main.expected in all three columns"
# and no cspec_tj.json: `main.js` does not time itself, so it has no column
# interleave.py could read. Its three builds exist for the cmp above.
echo "built"

# --- 3. THE SPECS ------------------------------------------------------------
python3 - "$OUT" <<'SPEC'
import json, os, sys
out = sys.argv[1]
cols = {"es": ["hand", "inf"], "m4": ["hand", "inf", "infobs"], "nl": ["hand", "inf"]}
for key, tags in cols.items():
    json.dump([{"name": t, "exe": f"./{key}_{t}.exe"} for t in tags],
              open(os.path.join(out, f"cspec_{key}.json"), "w"), indent=1)
json.dump([{"name": t, "exe": f"./tm_{t}.exe"} for t in ["none", "hand", "inf", "infobs"]],
          open(os.path.join(out, "cspec_tm.json"), "w"), indent=1)
json.dump([{"name": t, "exe": f"./cc_{t}.exe"} for t in ["hand", "inf"]],
          open(os.path.join(out, "cspec_cc.json"), "w"), indent=1)
SPEC
echo "specs written to $OUT"

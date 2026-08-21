#!/usr/bin/env bash
# Every configuration a newly pinned oracle case has to answer identically in.
#
# The harness (tests/oracle/oracle_test.cpp) runs four of these — infer and
# no-infer, each with and without GC stress — because those four are the ones
# every case in the ratchet must pass on every build. The rest are the ones a
# CHUNK owes the mechanism it just landed: its own seam alone, every seam of
# the chunk together, and the heap's verify+poison mode with and without
# stress. Automating them into ctest would make the full suite pay for one
# chunk's evidence forever; running them from here makes the evidence exact
# and leaves the suite the size it is.
#
#   tests/oracle/pin_matrix.sh <case-name> [<case-name> ...]
#
# Env: BRONZE=<path to bronze.exe> (default build/dev/src/cli/bronze.exe)
#      SEAMS="BRONZE_NO_X BRONZE_NO_Y" — the seams to sweep, singly and together
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BRONZE="${BRONZE:-$root/build/dev/src/cli/bronze.exe}"
SEAMS="${SEAMS:-BRONZE_NO_INLINE_ROOTS BRONZE_NO_STRICT_EQ_INLINE BRONZE_NO_ELEM_INLINE}"
work="${TMPDIR:-/tmp}/bronze_pin_matrix"
mkdir -p "$work"

fail=0
for name in "$@"; do
    js="$root/tests/oracle/cases/$name.js"
    expected="$root/tests/oracle/cases/$name.expected"
    if [ ! -f "$js" ] || [ ! -f "$expected" ]; then
        echo "MISSING $name"
        fail=1
        continue
    fi

    for mode in infer no-infer; do
        exe="$work/${name}_${mode}.exe"
        flag=""
        [ "$mode" = "no-infer" ] && flag="--no-infer"
        if ! "$BRONZE" build "$js" -o "$exe" $flag >"$work/build.log" 2>&1; then
            echo "BUILD-FAIL $name/$mode"
            cat "$work/build.log"
            fail=1
            continue
        fi

        # The environments, named so a failure says which one.
        envs=("plain:")
        envs+=("gc-stress:BRONZE_GC_STRESS=1")
        envs+=("verify-poison:BRONZE_HEAP_VERIFY=1 BRONZE_GC_POISON=1")
        envs+=("verify-poison-stress:BRONZE_HEAP_VERIFY=1 BRONZE_GC_POISON=1 BRONZE_GC_STRESS=1")
        all=""
        for seam in $SEAMS; do
            envs+=("$seam:$seam=1")
            envs+=("$seam+stress:$seam=1 BRONZE_GC_STRESS=1")
            all="$all $seam=1"
        done
        envs+=("all-seams:$all")
        envs+=("all-seams+stress:$all BRONZE_GC_STRESS=1")

        for spec in "${envs[@]}"; do
            label="${spec%%:*}"
            vars="${spec#*:}"
            out="$work/${name}_${mode}_${label}.out"
            # `export` with no arguments dumps the environment, so an empty
            # variable list must not reach it — "plain" is a real row.
            ( [ -n "$vars" ] && eval "export $vars"; "$exe" ) >"$out" 2>/dev/null
            if diff -q "$out" "$expected" >/dev/null 2>&1; then
                printf 'ok   %-28s %-8s %s\n' "$name" "$mode" "$label"
            else
                printf 'DIFF %-28s %-8s %s\n' "$name" "$mode" "$label"
                diff "$expected" "$out" | head -10
                fail=1
            fi
        done
    done
done

exit $fail

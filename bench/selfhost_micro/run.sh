#!/usr/bin/env bash
# Builds each micro benchmark here with `bronze build`, checks that its
# checksum matches node's, and prints user-space instructions (millions) for
# node and for the bronze executable, side by side.
#
#   bench/selfhost_micro/run.sh [--bronze <cli>] [--out <dir>] [name ...]
#
# Default CLI: build/src/cli/bronze. Default output: a temporary directory,
# removed at the end (one copy of the shared runtime, ~25 MB, plus a small
# module per benchmark). Names select benchmarks (without .js); none = all.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
bronze="$root/build/src/cli/bronze"
out=""
names=()
while [ $# -gt 0 ]; do
    case "$1" in
        --bronze) bronze="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        *) names+=("$1"); shift ;;
    esac
done
cleanup=0
if [ -z "$out" ]; then
    out="$(mktemp -d)"
    cleanup=1
fi
mkdir -p "$out"
if [ ${#names[@]} -eq 0 ]; then
    for f in "$here"/*.js; do names+=("$(basename "$f" .js)"); done
fi

instructions() {
    # perf stat's CSV: value,unit,event,...
    perf stat -x, -e instructions:u "$@" 2>&1 >/dev/null | awk -F, '/instructions/ { printf "%.0f", $1 / 1e6 }'
}

printf "%-18s %10s %10s %7s\n" "benchmark" "bronze M" "node M" "ratio"
status=0
for n in "${names[@]}"; do
    src="$here/$n.js"
    if ! "$bronze" build "$src" -o "$out/$n" >"$out/$n.build.log" 2>&1; then
        printf "%-18s build failed (%s)\n" "$n" "$out/$n.build.log"
        status=1
        continue
    fi
    want="$(node "$src")"
    got="$("$out/$n")"
    if [ "$want" != "$got" ]; then
        printf "%-18s checksum differs: node %s, bronze %s\n" "$n" "$want" "$got"
        status=1
        continue
    fi
    b="$(instructions "$out/$n")"
    nd="$(instructions node "$src")"
    printf "%-18s %10s %10s %7s\n" "$n" "$b" "$nd" "$(awk -v b="$b" -v n="$nd" 'BEGIN { printf "%.2f", b / n }')"
done
[ "$cleanup" = 1 ] && rm -rf "$out"
exit $status

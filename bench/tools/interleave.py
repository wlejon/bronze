# The measurement protocol this campaign's numbers are taken under, in one
# place, because every stage from 3.1 on has re-implemented it and stage E3
# found that the ROUND COUNT it had inherited was too small to decide anything
# about the configurations it was comparing.
#
#   python bench/tools/interleave.py <spec.json> [rounds]
#
# A spec is a list of columns:
#
#   [{"name": "E4 shipped", "exe": "./a_b.exe", "small": "twin", "iters": 5400000},
#    {"name": "twin",       "exe": "./a_s.exe"},
#    {"name": "three_math", "exe": "./tm.exe"}]
#
# A column with "small" is a TWO-COUNT column: its ns/iter is the wall delta
# against the named twin, over "iters" iterations, which cancels process
# startup exactly. A column without one is quoted raw in milliseconds.
#
# THE THREE RULES, and they are not optional:
#
#   - INTERLEAVED. One run of every column per round, so a drift in machine
#     state lands on every column equally. Never all of column A then all of B.
#   - MEDIANS, warmup round discarded. The mean is at the mercy of one
#     scheduling hiccup and these processes are milliseconds long.
#   - 101 ROUNDS for a kernel. Stage E3 measured three 13-round repeats of one
#     cell at 17.40, 15.23 and 14.92 ns and two 101-round repeats of the whole
#     spec agreeing to 0.14 ns on every cell. A 13-round A/B systematically
#     over-read the mechanism whose binary was bigger. 51 is enough for the
#     millisecond fixtures, whose own width is about ±1 ms.
#
# The checksum every column printed is reported beside its median, because a
# speedup with a changed checksum is a miscompile and this is where it shows.

import json
import statistics
import subprocess
import sys
import time


def run_once(exe):
    t0 = time.perf_counter()
    p = subprocess.run([exe], capture_output=True, text=True)
    t1 = time.perf_counter()
    return (t1 - t0) * 1000.0, p.stdout.strip()


def interleaved(cols, rounds=101, warmup=1):
    """cols: [(name, exe)] -> (name -> median ms, name -> last stdout)."""
    samples = {n: [] for n, _ in cols}
    out = {}
    for r in range(rounds + warmup):
        for n, e in cols:
            ms, so = run_once(e)
            if r >= warmup:
                samples[n].append(ms)
            out[n] = so
    return {n: statistics.median(v) for n, v in samples.items()}, out


def main():
    spec = json.load(open(sys.argv[1]))
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 101
    cols = [(c["name"], c["exe"]) for c in spec]
    med, out = interleaved(cols, rounds)
    for c in spec:
        n = c["name"]
        print(f"{n}\t{med[n]:.3f} ms\t{out[n]}")
    print()
    for c in spec:
        if "small" in c:
            d = med[c["name"]] - med[c["small"]]
            print(f"{c['name']}: {d * 1e6 / c['iters']:.3f} ns/iter   (delta {d:.3f} ms)")


if __name__ == "__main__":
    main()

# The node oracle column, under the same protocol as interleave.py — because a
# node number taken in another session is not a comparison. Node's process
# floor on a developer box is around 32 ms against a bronze exe's 5.6, so the
# two-count delta is the ONLY honest way to read a kernel across the two
# engines; a raw millisecond total is not.
#
#   python bench/tools/nodebench.py <spec.json> [rounds]
#
# The spec is interleave.py's, with "js" naming a source file instead of "exe"
# naming an executable.

import json
import statistics
import subprocess
import sys
import time


def run_once(js):
    t0 = time.perf_counter()
    p = subprocess.run(["node", js], capture_output=True, text=True)
    t1 = time.perf_counter()
    return (t1 - t0) * 1000.0, p.stdout.strip()


def main():
    spec = json.load(open(sys.argv[1]))
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 41
    samples = {c["name"]: [] for c in spec}
    out = {}
    for r in range(rounds + 1):
        for c in spec:
            ms, so = run_once(c["js"])
            if r >= 1:
                samples[c["name"]].append(ms)
            out[c["name"]] = so
    med = {n: statistics.median(v) for n, v in samples.items()}
    for c in spec:
        print(f"{c['name']}\t{med[c['name']]:.3f} ms\t{out[c['name']]}")
    print()
    for c in spec:
        if "small" in c:
            d = med[c["name"]] - med[c["small"]]
            print(f"{c['name']}: {d * 1e6 / c['iters']:.3f} ns/iter   (delta {d:.3f} ms)")


if __name__ == "__main__":
    main()

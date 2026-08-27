# The measurement protocol this suite's numbers are taken under, in one place.
#
#   python bench/tools/interleave.py <spec.json> [rounds]
#
# A spec is a list of columns, each naming a thing to run:
#
#   [{"name": "R2",   "exe": "./tm_r2.exe"},
#    {"name": "seam", "exe": "./tm_seam.exe"},
#    {"name": "node", "js": "three_math.js"}]
#
# A column with "exe" is run as a program; a column with "js" is run under node,
# out of band, and exists so a node number is taken in the SAME session as the
# bronze ones it is compared against. `bench/run_benchmarks.sh` never emits a
# "js" column — CLAUDE.md's rule is that the automated runner does not invoke
# node — and this tool is the manual instrument where that comparison is made.
#
# WHAT IS MEASURED IS WHAT THE FIXTURE PRINTED, not how long its process took.
# Every fixture times its own compute region through bench/harness.js and writes
#
#   [bench] <region> ms=<f> [ns_per_iter=<f>]
#
# to stderr. A process wall clock would add each engine's startup to that — 5.9
# ms for a bronze exe on this box and 31 ms for node, a gap five times the size
# of most of the differences this suite exists to detect — so the wall clock is
# not consulted at all. A fixture that prints no such line is an error, not a
# zero: it means the fixture was never converted.
#
# THE THREE RULES, and they are not optional:
#
#   - INTERLEAVED. One run of every column per round, so a drift in machine
#     state lands on every column equally. Never all of column A then all of B.
#   - MEDIANS, warmup round discarded. The mean is at the mercy of one
#     scheduling hiccup.
#   - 101 ROUNDS for a kernel, 51 for a millisecond fixture. A 13-round A/B was
#     measured to systematically over-read whichever arm had the bigger binary;
#     two 101-round repeats of one spec agree to within 1%.
#
# The checksum every column printed on STDOUT is reported beside its median,
# because a speedup with a changed checksum is a miscompile and this is where it
# shows.

import json
import re
import statistics
import subprocess
import sys

# `[bench] three_math ms=14.0231 ns_per_iter=2.903`
LINE = re.compile(r"^\[bench\] (\S+) ms=([0-9.]+)(?: ns_per_iter=([0-9.]+))?", re.M)


def run_once(col):
    """One run of one column -> ({region: ms}, {region: ns_per_iter}, stdout)."""
    cmd = ["node", col["js"]] if "js" in col else [col["exe"]]
    p = subprocess.run(cmd, capture_output=True, text=True)
    ms, per = {}, {}
    for region, msv, perv in LINE.findall(p.stderr):
        ms[region] = float(msv)
        if perv:
            per[region] = float(perv)
    if not ms:
        raise SystemExit(
            f"{col['name']}: printed no '[bench] ... ms=' line.\n"
            f"  ran: {' '.join(cmd)}\n"
            f"  stderr: {p.stderr.strip()[:400]}\n"
            "A fixture must time itself through bench/harness.js."
        )
    return ms, per, p.stdout.strip()


def main():
    spec = json.load(open(sys.argv[1]))
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 101

    # region key is (column name, region name) so a fixture with more than one
    # timed region — typed_array_loop, call_chain_kernel — keeps them apart.
    samples, pers, out = {}, {}, {}
    for r in range(rounds + 1):
        for col in spec:
            ms, per, so = run_once(col)
            if r >= 1:
                for region, v in ms.items():
                    samples.setdefault((col["name"], region), []).append(v)
                for region, v in per.items():
                    pers.setdefault((col["name"], region), []).append(v)
            out[col["name"]] = so

    width = max(len(f"{c}/{r}") for c, r in samples) + 2
    for col in spec:
        name = col["name"]
        for (c, region), vals in samples.items():
            if c != name:
                continue
            label = region if region == name else f"{name}/{region}"
            per = pers.get((c, region))
            tail = f"  {statistics.median(per):8.3f} ns/iter" if per else " " * 19
            print(f"{label:<{width}} {statistics.median(vals):9.3f} ms{tail}   "
                  f"{out[name].replace(chr(10), ' | ')}")


if __name__ == "__main__":
    main()

# `call_chain_kernel` times its two loops IN PROCESS and prints the nanoseconds
# itself, so a wall delta would only add this harness's own noise to a number
# the fixture already measured. This runs the columns interleaved, the way
# interleave.py does, and takes the median of what each one PRINTED.
#
#   python bench/tools/selftimed.py <exe> [<exe> ...] [--rounds N]
#
# The line it reads is `... chained_ns=<f> flat_ns=<f> ratio=<f>`.

import re
import statistics
import subprocess
import sys

PAT = re.compile(r"chained_ns=([0-9.]+) flat_ns=([0-9.]+)")


def main():
    args = sys.argv[1:]
    rounds = 21
    if "--rounds" in args:
        i = args.index("--rounds")
        rounds = int(args[i + 1])
        del args[i:i + 2]
    chained = {e: [] for e in args}
    flat = {e: [] for e in args}
    tail = {}
    for r in range(rounds + 1):
        for e in args:
            p = subprocess.run([e], capture_output=True, text=True)
            m = PAT.search(p.stdout)
            if r >= 1 and m:
                chained[e].append(float(m.group(1)))
                flat[e].append(float(m.group(2)))
            tail[e] = p.stdout.strip().splitlines()[0] if p.stdout.strip() else ""
    for e in args:
        c = statistics.median(chained[e])
        f = statistics.median(flat[e])
        print(f"{e}\tchained {c:.2f} ns\tflat {f:.2f} ns\tratio {c / f:.2f}\t{tail[e]}")


if __name__ == "__main__":
    main()

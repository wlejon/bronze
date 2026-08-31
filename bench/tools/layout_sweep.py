# BINARY LAYOUT AS A MEASURED VARIABLE, not a hidden one.
#
#   python bench/tools/layout_sweep.py <spec.json>
#
# The problem this exists for: a large fixture is emitted as sixteen partition
# objects, and the order they are handed to the linker decides where every
# function lands in the image. That placement is worth percent-scale amounts on
# a fixture like instanced_mesh_churn — comparable to the differences this
# suite exists to detect. `bench/tools/interleave.py` controls machine drift and
# `ab.sh` controls per-instance offsets; neither controls layout, so an A/B of
# two compilers pins one arbitrary layout per arm and reads the difference
# between those two draws as a result. Editing four NEVER-EXECUTED lines of a
# vendored bundle has been measured moving a region by +6% that way.
#
# So layout becomes an input. `bronze build --keep-objs <dir>` leaves the
# objects behind and `bronze link <dir> --link-seed <n>` relinks them under a
# deterministic permutation of the object order — seconds, against minutes for
# the compile — so one arm can be measured under S layouts instead of one. What
# the sweep reports per arm is then a POOLED median across seeds and, next to
# it, the CROSS-SEED SPREAD: how far apart the same code reads under nothing
# but a different placement.
#
# THE DECISION RULE, which the tool prints with every result:
#
#   An arm-vs-arm delta is CLAIMABLE only if it exceeds the WIDER of the two
#   arms' cross-seed spreads. A delta smaller than that is inside the range one
#   arm covers on its own, and no amount of extra rounds shrinks it — rounds
#   average out machine noise, and this is not machine noise.
#
# The three rules of interleave.py still hold and are inherited here: one run of
# every column per round, medians not means, and the stdout checksum reported
# beside the number, because a speedup with a changed checksum is a miscompile.
# A column here is one (arm, seed) pair, and each column is staged as several
# independently-created copies of its exe because two copies of one binary read
# up to ~6.5% apart on this box.
#
# SPEC:
#
#   {"region": "instanced_mesh_churn",
#    "seeds": [1, 2, 3, 4, 5, 6, 7, 8],
#    "rounds": 7,
#    "copies": 3,
#    "bronze": "D:/projects/bronze/build/dev/src/cli/bronze.exe",
#    "launcher": ["D:/projects/bronze/dev.cmd"],
#    "stage": "C:/.../stage",
#    "arms": [{"name": "base", "entry": ".../imc.js", "objs": ".../objs/base"},
#             {"name": "edit", "objs": ".../objs/edit"}]}
#
# An arm with "entry" is compiled once into its "objs" directory; an arm with
# only "objs" reuses a directory an earlier run filled. "launcher" is prefixed
# to every bronze invocation (on Windows the toolchain lives behind dev.cmd).

import json
import os
import re
import shutil
import statistics
import subprocess
import sys

LINE = re.compile(r"^\[bench\] (\S+) ms=([0-9.]+)", re.M)


def run_bronze(spec, args):
    cmd = list(spec.get("launcher", [])) + [spec["bronze"]] + args
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"bronze failed: {' '.join(cmd)}\n{p.stdout}\n{p.stderr}")


def build_arm(spec, arm):
    """Compile once. The objects are the artefact; the exe from this step is
    thrown away, because a build-time link names its objects by temp path and a
    relink names them part000.obj — different bytes for the same program, and
    every arm must come off the same path to be comparable."""
    run_bronze(spec, ["build", arm["entry"], "-o", os.path.join(spec["stage"], arm["name"] + "_build.exe"),
                      "--keep-objs", arm["objs"]])


def link_seed(spec, arm, seed):
    """`null` in the seeds list is the DEFAULT order — the one every build has
    used — so a sweep can show where today's number sits in the distribution
    rather than only how wide the distribution is."""
    exe = os.path.join(spec["stage"], f"{arm['name']}_s{seed}.exe")
    args = ["link", arm["objs"], "-o", exe]
    if seed is not None:
        args += ["--link-seed", str(seed)]
    run_bronze(spec, args)
    return exe


def run_once(exe, region):
    p = subprocess.run([exe], capture_output=True, text=True)
    found = {r: float(v) for r, v in LINE.findall(p.stderr)}
    if region not in found:
        raise SystemExit(
            f"{exe}: printed no '[bench] {region} ms=' line.\n"
            f"  stderr: {p.stderr.strip()[:400]}\n"
            "A fixture must time itself through bench/harness.js."
        )
    return found[region], p.stdout.strip()


def spread(values):
    return max(values) - min(values)


def main():
    spec = json.load(open(sys.argv[1]))
    region = spec["region"]
    seeds = spec["seeds"]
    rounds = int(spec.get("rounds", 7))
    copies = int(spec.get("copies", 3))
    stage = spec["stage"]
    os.makedirs(stage, exist_ok=True)

    # Build, then link every (arm, seed). Columns are created in this order and
    # RUN interleaved below, so a drift in machine state lands on all of them.
    columns = []
    for arm in spec["arms"]:
        if arm.get("entry"):
            build_arm(spec, arm)
        for seed in seeds:
            exe = link_seed(spec, arm, seed)
            staged = []
            for c in range(copies):
                dst = os.path.join(stage, f"{arm['name']}_s{seed}_c{c}.exe")
                shutil.copyfile(exe, dst)
                staged.append(dst)
            columns.append({"arm": arm["name"], "seed": seed, "copies": staged, "ms": []})

    checksum = None
    void = []
    for r in range(rounds + 1):
        for col in columns:
            ms, out = run_once(col["copies"][r % copies], region)
            if checksum is None:
                checksum = out
            if out != checksum:
                void.append(f"{col['arm']}/seed{col['seed']} round {r}: {out!r}")
            if r >= 1:  # warmup round discarded, as everywhere in this suite
                col["ms"].append(ms)

    print(f"region={region} seeds={len(seeds)} rounds={rounds} copies={copies} "
          f"runs/column={rounds}")
    print(f"checksum={checksum}")
    if void:
        print("RESULT: VOID - the arms did not compute the same thing:")
        for v in void:
            print("  " + v)

    results = {}
    labels = ["default" if s is None else f"s{s}" for s in seeds]
    seed_width = max(9, 2 + max(len(x) for x in labels))
    header = "arm".ljust(14) + "".join(x.rjust(seed_width) for x in labels)
    print("\nper-seed medians (ms)")
    print(header + "med^2".rjust(9) + "mean".rjust(9) + "spread".rjust(9) + "spread%".rjust(9))
    for arm in spec["arms"]:
        per_seed = [statistics.median([c["ms"] for c in columns
                                       if c["arm"] == arm["name"] and c["seed"] == s][0])
                    for s in seeds]
        # Two centres, because they disagree about what a seed is. The
        # median-of-medians throws away every seed but the middle one, which on
        # nine seeds moves by whole percent between repeats of the same sweep;
        # the mean of the per-seed medians spends all of them, and each is
        # already robust to the noise WITHIN its seed. A delta has to clear the
        # bar on both before it is called.
        pooled = statistics.median(per_seed)
        mean = statistics.fmean(per_seed)
        sp = spread(per_seed)
        results[arm["name"]] = (pooled, mean, sp)
        row = arm["name"].ljust(14) + "".join(f"{v:{seed_width}.2f}" for v in per_seed)
        print(row + f"{pooled:9.2f}{mean:9.2f}{sp:9.2f}{100.0 * sp / mean:8.1f}%")

    print("\nDECISION RULE: an arm-vs-arm delta is CLAIMABLE only if it exceeds the")
    print("wider of the two arms' cross-seed spreads, on BOTH centres. A smaller")
    print("delta is inside the range one arm covers under nothing but a different")
    print("binary layout, and more rounds will not shrink it: rounds average out")
    print("machine noise, and this is not machine noise.")
    names = [a["name"] for a in spec["arms"]]
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            a, b = names[i], names[j]
            d_med = results[b][0] - results[a][0]
            d_mean = results[b][1] - results[a][1]
            bar = max(results[a][2], results[b][2])
            verdict = "CLAIMABLE" if min(abs(d_med), abs(d_mean)) > bar else "NOT CLAIMABLE"
            print(f"  {b} vs {a}: med^2 {d_med:+.2f} ms ({100.0 * d_med / results[a][0]:+.1f}%)"
                  f"  mean {d_mean:+.2f} ms ({100.0 * d_mean / results[a][1]:+.1f}%)"
                  f"  bar={bar:.2f} ms  ->  {verdict}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Summarize a type-feedback profile (`bronze build --feedback-out`, docs/type-feedback.md).

    python3 bench/tools/feedback_report.py run.tfb [--top 20] [--json]

What it answers, each as a share of EXECUTIONS (a site's count), not of sites:

  dynamic ops   value sites (operands of arithmetic, compares, truthiness,
                ToNumber): monomorphic = one tag; number-mono = only f64/i32,
                which a specialization treats as one kind with an int check
  IC sites      shape and method sites: one receiver shape
  call sites    target and method sites: one callee (also counted per site)
  per function  the hottest functions by entry count, with their dynamic-op
                and IC shares, so a reader sees where the polymorphism is

Pure stdlib; reads the file format the runtime writes and nothing else.
"""

import argparse
import json
import sys

# Version 2 lists every key of a shape and may carry a harvest (ic rows,
# docs/type-feedback.md "Harvest"), which has no counts to report; version 3
# adds the harvest's `other` rows.
VERSIONS = ("bronze-type-feedback 1", "bronze-type-feedback 2", "bronze-type-feedback 3")
NUMBER_TAGS = {"f64", "i32"}


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            out.append({"\\": "\\", "t": "\t", "n": "\n", "r": "\r"}.get(s[i + 1], s[i + 1]))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def parse(path):
    fns = {}
    fn = None
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    header = False
    for n, line in enumerate(lines, 1):
        if not line or line.startswith("#"):
            continue
        if not header:
            if line not in VERSIONS:
                sys.exit(f"{path}:{n}: expected '{VERSIONS[-1]}'")
            header = True
            continue
        f = line.split("\t")
        if f[0] == "fn":
            fn = {"key": unescape(f[1]), "entries": int(f[2]), "sites": {}}
            fns[fn["key"]] = fn
        elif f[0] == "site":
            tags = {}
            if len(f) > 4 and f[4]:
                for item in f[4].split(" "):
                    k, v = item.split("=")
                    tags[k] = int(v)
            fn["sites"][int(f[1])] = {"kind": f[2], "total": int(f[3]), "tags": tags,
                                     "shapes": [], "targets": []}
        elif f[0] in ("shape", "target"):
            fn["sites"][int(f[1])][f[0] + "s"].append((unescape(f[3]), int(f[2])))
        elif f[0] in ("harvest", "ic", "way"):
            continue
        else:
            sys.exit(f"{path}:{n}: unknown row '{f[0]}'")
    return fns


def sole(names):
    return len(names) == 1 and names[0][0] != "*other*"


class Tally:
    def __init__(self):
        self.execs = 0
        self.hit = 0
        self.sites = 0
        self.site_hit = 0

    def add(self, total, ok):
        self.execs += total
        self.sites += 1
        if ok:
            self.hit += total
            self.site_hit += 1

    def share(self):
        return self.hit / self.execs if self.execs else None

    def as_dict(self):
        return {"executions": self.execs, "hit": self.hit, "share": self.share(),
                "sites": self.sites, "sites_hit": self.site_hit}


def pct(x):
    return "   -  " if x is None else f"{100 * x:5.1f}%"


def analyze(fns):
    total = {"dyn_mono": Tally(), "dyn_num": Tally(), "ic_mono": Tally(), "call_mono": Tally()}
    per_fn = {}
    for key, fn in fns.items():
        mine = {"dyn_mono": Tally(), "dyn_num": Tally(), "ic_mono": Tally(), "call_mono": Tally()}
        for site in fn["sites"].values():
            t = site["total"]
            if t == 0:
                continue
            kind = site["kind"]
            tags = [k for k, v in site["tags"].items() if v]
            if kind == "value":
                for tally in (total, mine):
                    tally["dyn_mono"].add(t, len(tags) == 1)
                    tally["dyn_num"].add(t, len(tags) == 1 or set(tags) <= NUMBER_TAGS)
            if kind in ("shape", "method"):
                for tally in (total, mine):
                    tally["ic_mono"].add(t, sole(site["shapes"]))
            if kind in ("target", "method") and site["targets"]:
                for tally in (total, mine):
                    tally["call_mono"].add(t, sole(site["targets"]))
        per_fn[key] = mine
    return total, per_fn


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--top", type=int, default=20, help="hot functions to list (by entry count)")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    fns = parse(args.profile)
    total, per_fn = analyze(fns)
    hot = sorted(fns.values(), key=lambda f: (-f["entries"], f["key"]))[: args.top]

    if args.json:
        print(json.dumps({
            "functions": len(fns),
            "totals": {k: v.as_dict() for k, v in total.items()},
            "hot": [{"key": f["key"], "entries": f["entries"],
                     **{k: v.as_dict() for k, v in per_fn[f["key"]].items()}} for f in hot],
        }, indent=2))
        return

    print(f"{args.profile}: {len(fns)} functions")
    rows = [
        ("dynamic-op executions, one tag", total["dyn_mono"]),
        ("dynamic-op executions, numbers only", total["dyn_num"]),
        ("IC executions, one shape", total["ic_mono"]),
        ("call executions, one target", total["call_mono"]),
    ]
    for label, t in rows:
        print(f"  {label:40s} {pct(t.share())}  of {t.execs:>12,} execs"
              f"   ({t.site_hit:,}/{t.sites:,} sites)")
    print()
    print(f"  {'entries':>10}  {'1-tag':>6}  {'num':>6}  {'1-shape':>7}  {'1-tgt':>6}  function")
    for f in hot:
        m = per_fn[f["key"]]
        print(f"  {f['entries']:>10,}  {pct(m['dyn_mono'].share())}  {pct(m['dyn_num'].share())}"
              f"  {pct(m['ic_mono'].share()):>7}  {pct(m['call_mono'].share())}  {f['key']}")


if __name__ == "__main__":
    main()

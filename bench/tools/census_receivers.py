"""Per-receiver join of a shape census (BRONZE_SHAPE_CENSUS=1, dynamic) with
`bronze build --wpo-stats=<fns>` (static), on the IC index both print.

    python3 census_receivers.py <census.json> <wpo-stats.txt> fn[,fn...]

For each named function (the busiest of several with one name) and each
receiver SSA value it reads through: its keyed sites, dynamic accesses per
activation, the distinct receiver shapes the census saw at those sites and the
top one's share, the predicting solve's classes for the sites, and, between
consecutive accesses of the receiver in block order (weighted by the later
access's hits), the calls (`C`/`M` tokens) and the other receivers' IC sites
without a static slot (each a stub call that may run a getter).

Activations are the busiest keyed site of the entry block (else of the whole
function). Census sites are keyed by IC index only for rows symbolized into the
compiled module (`--module`, default app.so). docs/shape-census.md, "Joining
with the build". Manual analysis tooling; nothing runs it automatically."""
import json
import re
import sys
from collections import Counter, defaultdict

args = [a for a in sys.argv[1:] if not a.startswith("--module=")]
module = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--module=")), "app.so")
census_path, wpo_path, fns = args[0], args[1], args[2].split(",")
census = json.load(open(census_path))
wpo = open(wpo_path).read().splitlines()

by_ic = {s["ic"]: s for s in census["sites"] if module in s["module"] and s.get("ic", -1) >= 0}


def hits(ic):
    s = by_ic.get(ic)
    return s["hits"] if s else 0


site_re = re.compile(r"^wpo-pred-site (\S+) b\d+:\d+ \S+ \.\S* (?:\[inlined\] )?-> (\S+).* ic (\d+) r%\d+$")
cls_of = {}
for line in wpo:
    m = site_re.match(line)
    if m:
        cls_of[int(m.group(3))] = m.group(2)

tok_re = re.compile(r"^([GSM])(\d+)@(\d+)(s?)$")


def tokens(parts):
    out, blk = [], 0
    for t in parts:
        if t.startswith("|"):
            blk = int(t[1:])
        elif t == "C":
            out.append(("C", None, None, False, blk))
        else:
            m = tok_re.match(t)
            if m:
                out.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4) == "s", blk))
    return out


seqs = {}
for line in wpo:
    if line.startswith("wpo-pred-seq "):
        parts = line.split()
        toks = tokens(parts[2:])
        weight = sum(hits(t[2]) for t in toks if t[0] != "C")
        if parts[1] not in seqs or weight > seqs[parts[1]][0]:
            seqs[parts[1]] = (weight, toks)

for fn in fns:
    if fn not in seqs:
        print(f"== {fn}: not in the --wpo-stats filter")
        continue
    toks = seqs[fn][1]
    keyed = [t for t in toks if t[0] != "C"]
    acts = max([hits(t[2]) for t in keyed if t[4] == 0] or [0]) or max([hits(t[2]) for t in keyed] or [0])
    per = defaultdict(lambda: {"sites": 0, "hits": 0, "shapes": Counter(), "cls": Counter(), "gaps": []})
    last = {}
    for idx, (kind, recv, ic, static, _blk) in enumerate(toks):
        if kind == "C":
            continue
        p = per[recv]
        p["sites"] += 1
        h = hits(ic)
        p["hits"] += h
        for row in by_ic.get(ic, {}).get("shapes", []):
            p["shapes"][row["shape"]] += row["count"]
        p["cls"][cls_of.get(ic, "?")] += 1
        if recv in last:
            between = toks[last[recv] + 1:idx]
            calls = sum(1 for t in between if t[0] in ("C", "M")) + (toks[last[recv]][0] == "M")
            stubs = sum(1 for t in between if t[0] in ("G", "S") and not t[3] and t[1] != recv)
            p["gaps"].append((h, calls, stubs))
        last[recv] = idx
    total = sum(p["hits"] for p in per.values())
    print(f"== {fn}: activations ~{acts}, keyed sites {len(keyed)}, receivers {len(per)}, "
          f"accesses/activation {total / max(acts, 1):.1f}")
    for recv, p in sorted(per.items(), key=lambda kv: -kv[1]["hits"])[:12]:
        if p["hits"] == 0:
            continue
        shapes = p["shapes"]
        seen = sum(shapes.values())
        top = shapes.most_common(1)[0][1] / seen if seen else 0.0
        w = sum(g[0] for g in p["gaps"])
        calls = sum(g[0] * g[1] for g in p["gaps"]) / w if w else float("nan")
        stubs = sum(g[0] * g[2] for g in p["gaps"]) / w if w else float("nan")
        cls = ",".join(f"{c}:{n}" for c, n in p["cls"].most_common(3))
        print(f"  r%{recv:<5} sites {p['sites']:3} acc/act {p['hits'] / max(acts, 1):6.2f} shapes {len(shapes):2} "
              f"top {top:4.2f} calls-between {calls:5.2f} ic-stubs-between {stubs:5.2f} | {cls}")

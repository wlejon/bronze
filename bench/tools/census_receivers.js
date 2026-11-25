// Per-receiver join of a shape census (BRONZE_SHAPE_CENSUS=1, dynamic) with
// `bronze build --wpo-stats=<fns>` (static), on the IC index both print.
//
//   bronze run bench/tools/census_receivers.js -- [--module=app.so] <census.json> <wpo-stats.txt> fn[,fn...]

const rawArgs = Bronze.args;
let modName = 'app.so';
const pos = [];
for (const a of rawArgs) {
  if (a.startsWith('--module=')) modName = a.slice(9);
  else pos.push(a);
}
if (pos.length < 3) {
  console.error('usage: census_receivers [--module=app.so] <census.json> <wpo-stats.txt> fn[,fn...]');
  Bronze.exit(2);
}

const [censusPath, wpoPath, fnsArg] = pos;
const fns = fnsArg.split(',');
const census = JSON.parse(Bronze.fs.readText(censusPath));
const wpo = Bronze.fs.readText(wpoPath).replace(/\r\n/g, '\n').split('\n');

const byIc = new Map();
for (const s of census.sites || []) {
  if (s.module && s.module.includes(modName) && (s.ic ?? -1) >= 0) {
    byIc.set(s.ic, s);
  }
}

function hits(ic) {
  const s = byIc.get(ic);
  return s ? s.hits : 0;
}

const siteRe = /^wpo-pred-site (\S+) b\d+:\d+ \S+ \.\S* (?:\[inlined\] )?-> (\S+).* ic (\d+) r%\d+$/;
const clsOf = new Map();
for (const line of wpo) {
  const m = siteRe.exec(line);
  if (m) clsOf.set(parseInt(m[3], 10), m[2]);
}

const tokRe = /^([GSM])(\d+)@(\d+)(s?)$/;
function parseTokens(parts) {
  const out = [];
  let blk = 0;
  for (const t of parts) {
    if (t.startsWith('|')) {
      blk = parseInt(t.slice(1), 10);
    } else if (t === 'C') {
      out.push(['C', null, null, false, blk]);
    } else {
      const m = tokRe.exec(t);
      if (m) out.push([m[1], parseInt(m[2], 10), parseInt(m[3], 10), m[4] === 's', blk]);
    }
  }
  return out;
}

const seqs = new Map();
for (const line of wpo) {
  if (!line.startsWith('wpo-pred-seq ')) continue;
  const parts = line.trim().split(/\s+/);
  const toks = parseTokens(parts.slice(2));
  let weight = 0;
  for (const t of toks) {
    if (t[0] !== 'C') weight += hits(t[2]);
  }
  const prev = seqs.get(parts[1]);
  if (!prev || weight > prev.weight) seqs.set(parts[1], { weight, toks });
}

function bump(map, k, n = 1) {
  map.set(k, (map.get(k) || 0) + n);
}

for (const fn of fns) {
  if (!seqs.has(fn)) {
    console.log(`== ${fn}: not in the --wpo-stats filter`);
    continue;
  }
  const toks = seqs.get(fn).toks;
  const keyed = toks.filter(t => t[0] !== 'C');
  let acts = 0;
  for (const t of keyed) {
    if (t[4] === 0) acts = Math.max(acts, hits(t[2]));
  }
  if (acts === 0) {
    for (const t of keyed) acts = Math.max(acts, hits(t[2]));
  }

  const per = new Map();
  const last = new Map();
  for (let idx = 0; idx < toks.length; idx++) {
    const [kind, recv, ic] = toks[idx];
    if (kind === 'C') continue;
    let p = per.get(recv);
    if (!p) {
      p = { sites: 0, hits: 0, shapes: new Map(), cls: new Map(), gaps: [] };
      per.set(recv, p);
    }
    p.sites += 1;
    const h = hits(ic);
    p.hits += h;
    const site = byIc.get(ic);
    if (site && site.shapes) {
      for (const row of site.shapes) bump(p.shapes, row.shape, row.count);
    }
    bump(p.cls, clsOf.get(ic) || '?', 1);
    if (last.has(recv)) {
      const prevIdx = last.get(recv);
      let calls = toks[prevIdx][0] === 'M' ? 1 : 0;
      let stubs = 0;
      for (let j = prevIdx + 1; j < idx; j++) {
        const t = toks[j];
        if (t[0] === 'C' || t[0] === 'M') calls += 1;
        if ((t[0] === 'G' || t[0] === 'S') && !t[3] && t[1] !== recv) stubs += 1;
      }
      p.gaps.push([h, calls, stubs]);
    }
    last.set(recv, idx);
  }

  let total = 0;
  for (const p of per.values()) total += p.hits;
  console.log(
    `== ${fn}: activations ~${acts}, keyed sites ${keyed.length}, receivers ${per.size}, ` +
    `accesses/activation ${(total / Math.max(acts, 1)).toFixed(1)}`
  );

  const ranked = Array.from(per.entries()).sort((a, b) => b[1].hits - a[1].hits).slice(0, 12);
  for (const [recv, p] of ranked) {
    if (p.hits === 0) continue;
    let seen = 0;
    let topCount = 0;
    for (const c of p.shapes.values()) {
      seen += c;
      if (c > topCount) topCount = c;
    }
    const top = seen ? topCount / seen : 0.0;
    let w = 0, callSum = 0, stubSum = 0;
    for (const g of p.gaps) {
      w += g[0];
      callSum += g[0] * g[1];
      stubSum += g[0] * g[2];
    }
    const calls = w ? callSum / w : NaN;
    const stubs = w ? stubSum / w : NaN;
    const cls = Array.from(p.cls.entries())
      .sort((a, b) => b[1] - a[1])
      .slice(0, 3)
      .map(([c, n]) => `${c}:${n}`)
      .join(',');
    console.log(
      `  r%${String(recv).padEnd(5)} sites ${String(p.sites).padStart(3)} ` +
      `acc/act ${(p.hits / Math.max(acts, 1)).toFixed(2).padStart(6)} ` +
      `shapes ${String(p.shapes.size).padStart(2)} top ${top.toFixed(2).padStart(4)} ` +
      `calls-between ${Number.isNaN(calls) ? '  nan' : calls.toFixed(2).padStart(5)} ` +
      `ic-stubs-between ${Number.isNaN(stubs) ? '  nan' : stubs.toFixed(2).padStart(5)} | ${cls}`
    );
  }
}

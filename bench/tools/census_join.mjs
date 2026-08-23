// Joins a BRONZE_SAMPLE profile with a BRONZE_SHAPE_CENSUS artifact and
// prints the campaign's monomorphization go/no-go inputs:
//   - top compiled-JS functions by self time (steady-state window when the
//     sampler emitted one), with per-frame ms when --frames is given;
//   - each function's census coverage (fraction of its property traffic at
//     monomorphic-in-practice sites);
//   - the single number: the fraction of inline compiled-code time sitting
//     in functions whose property traffic is monomorphic-in-practice.
//
// Manual analysis tooling. Never run by the automated bench runner (the
// no-node rule in CLAUDE.md covers the runner, not a human at a shell).
//
//   node census_join.mjs --sample bronze_sample.json --census census.json \
//        [--frames 300] [--top 15] [--module app.dll]

import fs from 'fs';

const args = {};
for (let i = 2; i < process.argv.length; i += 2) {
  args[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
}
const samplePath = args.sample || 'bronze_sample.json';
const censusPath = args.census || null;
const frames = args.frames ? +args.frames : 0;
const topN = args.top ? +args.top : 15;
const moduleName = args.module || 'app.dll';

const sample = JSON.parse(fs.readFileSync(samplePath, 'utf8'));
const census = censusPath ? JSON.parse(fs.readFileSync(censusPath, 'utf8')) : null;

// The split path promotes internal symbols to `__bronze_part$name` (PDBs
// strip one leading underscore); wrappers are `__wrapper_name`. Both spell
// the same JS function for a reader, so both fold onto the bare name.
const baseName = (n) =>
  n.replace(/^_*bronze_part\$/, '').replace(/^__wrapper_/, '').replace(/^_*wrapper_/, '');

const useTail = sample.tail_ms > 0;
const samples = useTail ? sample.samples_tail : sample.samples;
const periodMs = sample.period_ms;

// Aggregate sampler rows by base name within the chosen module.
const rows = new Map();
let moduleSelf = 0;
let allSelf = 0;
const moduleSelfByModule = new Map();
for (const f of sample.functions) {
  const self = useTail ? f.self_tail : f.self;
  const total = useTail ? f.total_tail : f.total;
  allSelf += self;
  moduleSelfByModule.set(f.module, (moduleSelfByModule.get(f.module) || 0) + self);
  if (f.module !== moduleName) continue;
  moduleSelf += self;
  const key = baseName(f.name);
  const row = rows.get(key) || { name: key, self: 0, total: 0 };
  row.self += self;
  row.total = Math.max(row.total, total);
  rows.set(key, row);
}

// Census coverage per base name.
const coverage = new Map();
if (census) {
  for (const fn of census.functions) {
    const key = baseName(fn.name);
    const cur = coverage.get(key) || { obs: 0, monoObs: 0, sites: 0, numberVals: 0 };
    cur.obs += fn.observations;
    cur.monoObs += fn.mono_observations;
    cur.sites += fn.sites;
    cur.numberVals += fn.number_vals;
    coverage.set(key, cur);
  }
}

const sorted = [...rows.values()].sort((a, b) => b.self - a.self);
const msOf = (n) => n * periodMs;
const perFrame = (n) => (frames ? msOf(n) / frames : 0);

console.log(`window: ${useTail ? `tail ${sample.tail_ms} ms` : 'full run'};` +
  ` samples ${samples}; period ${periodMs.toFixed(3)} ms` +
  (frames ? `; frames ${frames}` : ''));
console.log('\nmodule self-time split:');
for (const [m, s] of [...moduleSelfByModule.entries()].sort((a, b) => b[1] - a[1])) {
  if (s === 0) continue;
  console.log(`  ${m.padEnd(30)} ${msOf(s).toFixed(0).padStart(8)} ms  ${(100 * s / allSelf).toFixed(1).padStart(5)}%` +
    (frames ? `  ${(msOf(s) / frames).toFixed(3).padStart(8)} ms/frame` : ''));
}

console.log(`\ntop ${topN} ${moduleName} functions by self${useTail ? ' (tail window)' : ''}:`);
console.log('function'.padEnd(52) + 'self-ms'.padStart(9) + (frames ? 'ms/frame'.padStart(10) : '') +
  'self%mod'.padStart(10) + (census ? 'monoCov'.padStart(9) + 'numVal%'.padStart(9) : ''));
for (const row of sorted.slice(0, topN)) {
  const cov = coverage.get(row.name);
  console.log(
    row.name.slice(0, 50).padEnd(52) +
    msOf(row.self).toFixed(0).padStart(9) +
    (frames ? perFrame(row.self).toFixed(3).padStart(10) : '') +
    (100 * row.self / (moduleSelf || 1)).toFixed(1).padStart(9) + '%' +
    (census
      ? (cov ? (100 * cov.monoObs / (cov.obs || 1)).toFixed(1).padStart(8) + '%' : '     n/a') +
        (cov ? (100 * cov.numberVals / (cov.obs || 1)).toFixed(1).padStart(8) + '%' : '     n/a')
      : ''));
}

if (census) {
  // The go/no-go number: self-time-weighted monomorphic coverage over the
  // module's functions. A function absent from the census (no dynamic
  // property traffic — pure arithmetic) is counted at coverage 1: a
  // monomorphizer has nothing to break there.
  let wCov = 0, w = 0, wKnown = 0;
  for (const row of rows.values()) {
    const cov = coverage.get(row.name);
    const c = cov && cov.obs > 0 ? cov.monoObs / cov.obs : 1;
    wCov += row.self * c;
    w += row.self;
    if (cov && cov.obs > 0) wKnown += row.self;
  }
  console.log(`\nmonomorphic-coverage of ${moduleName} inline self time: ` +
    `${(100 * wCov / (w || 1)).toFixed(1)}%` +
    ` (census-observed functions carry ${(100 * wKnown / (w || 1)).toFixed(1)}% of that time)`);
  console.log(`census totals: sites ${census.total_sites}, observations ${census.total_observations}, ` +
    `mono observations ${census.mono_observations} (${(100 * census.mono_observations / census.total_observations).toFixed(2)}%)`);
  console.log('poly histogram (sites / observations): ' +
    Object.entries(census.poly_histogram).map(([k, v]) => `${k}:${v.sites}/${v.observations}`).join('  '));
}

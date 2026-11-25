// Summarize a type-feedback profile (`bronze build --feedback-out`, docs/type-feedback.md).
//
//   bronze run bench/tools/feedback_report.js -- run.tfb [--top 20] [--json]
//   # or compiled: bronze build bench/tools/feedback_report.js -o feedback-report

const VERSIONS = new Set([
  'bronze-type-feedback 1',
  'bronze-type-feedback 2',
  'bronze-type-feedback 3',
]);
const NUMBER_TAGS = new Set(['f64', 'i32']);

function unescapeField(s) {
  let out = '';
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c === '\\' && i + 1 < s.length) {
      const n = s[i + 1];
      if (n === '\\') out += '\\';
      else if (n === 't') out += '\t';
      else if (n === 'n') out += '\n';
      else if (n === 'r') out += '\r';
      else out += n;
      i++;
      continue;
    }
    out += c;
  }
  return out;
}

function parseProfile(path) {
  const fns = new Map();
  let fn = null;
  const text = Bronze.fs.readText(path).replace(/\r\n/g, '\n');
  const lines = text.split('\n');
  let header = false;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const lineNo = i + 1;
    if (!line || line.startsWith('#')) continue;
    if (!header) {
      if (!VERSIONS.has(line)) {
        console.error(`${path}:${lineNo}: expected 'bronze-type-feedback 3'`);
        Bronze.exit(1);
      }
      header = true;
      continue;
    }
    const f = line.split('\t');
    if (f[0] === 'fn') {
      fn = { key: unescapeField(f[1]), entries: parseInt(f[2], 10), sites: new Map() };
      fns.set(fn.key, fn);
    } else if (f[0] === 'site') {
      const tags = new Map();
      if (f.length > 4 && f[4]) {
        for (const item of f[4].split(' ')) {
          const eq = item.indexOf('=');
          if (eq >= 0) tags.set(item.slice(0, eq), parseInt(item.slice(eq + 1), 10));
        }
      }
      fn.sites.set(parseInt(f[1], 10), {
        kind: f[2],
        total: parseInt(f[3], 10),
        tags,
        shapes: [],
        targets: [],
      });
    } else if (f[0] === 'shape' || f[0] === 'target') {
      fn.sites.get(parseInt(f[1], 10))[f[0] + 's'].push([unescapeField(f[3]), parseInt(f[2], 10)]);
    } else if (f[0] === 'harvest' || f[0] === 'ic' || f[0] === 'way' || f[0] === 'other' ||
               f[0] === 'slowsites' || f[0] === 'slow' || f[0] === 'last' || f[0] === 'callee' ||
               f[0] === 'doubles' || f[0] === 'objects' || f[0] === 'mixed' || f[0] === 'nonobject') {
      continue;
    } else {
      console.error(`${path}:${lineNo}: unknown row '${f[0]}'`);
      Bronze.exit(1);
    }
  }
  return fns;
}

function sole(names) {
  return names.length === 1 && names[0][0] !== '*other*';
}

class Tally {
  constructor() {
    this.execs = 0;
    this.hit = 0;
    this.sites = 0;
    this.siteHit = 0;
  }
  add(total, ok) {
    this.execs += total;
    this.sites += 1;
    if (ok) {
      this.hit += total;
      this.siteHit += 1;
    }
  }
  share() {
    return this.execs ? this.hit / this.execs : null;
  }
  asObject() {
    return {
      executions: this.execs,
      hit: this.hit,
      share: this.share(),
      sites: this.sites,
      sites_hit: this.siteHit,
    };
  }
}

function pct(x) {
  return x === null ? '   -  ' : (100 * x).toFixed(1).padStart(5) + '%';
}

function fmtInt(n) {
  return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function makeTallies() {
  return {
    dyn_mono: new Tally(),
    dyn_num: new Tally(),
    ic_mono: new Tally(),
    call_mono: new Tally(),
  };
}

function analyze(fns) {
  const total = makeTallies();
  const perFn = new Map();
  for (const [key, fn] of fns.entries()) {
    const mine = makeTallies();
    for (const site of fn.sites.values()) {
      const t = site.total;
      if (t === 0) continue;
      const kind = site.kind;
      const tags = [];
      for (const [k, v] of site.tags.entries()) {
        if (v) tags.push(k);
      }
      if (kind === 'value') {
        const isMono = tags.length === 1;
        const isNum = isMono || tags.every(k => NUMBER_TAGS.has(k));
        for (const tally of [total, mine]) {
          tally.dyn_mono.add(t, isMono);
          tally.dyn_num.add(t, isNum);
        }
      }
      if (kind === 'shape' || kind === 'method') {
        const ok = sole(site.shapes);
        for (const tally of [total, mine]) tally.ic_mono.add(t, ok);
      }
      if ((kind === 'target' || kind === 'method') && site.targets.length > 0) {
        const ok = sole(site.targets);
        for (const tally of [total, mine]) tally.call_mono.add(t, ok);
      }
    }
    perFn.set(key, mine);
  }
  return { total, perFn };
}

const args = Bronze.args.slice();
let profilePath = '';
let top = 20;
let asJson = false;
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--json') asJson = true;
  else if (a === '--top' && i + 1 < args.length) top = parseInt(args[++i], 10);
  else if (a.startsWith('--top=')) top = parseInt(a.slice(6), 10);
  else if (!profilePath) profilePath = a;
}
if (!profilePath) {
  console.error('usage: feedback_report <run.tfb> [--top 20] [--json]');
  Bronze.exit(2);
}

const fns = parseProfile(profilePath);
const { total, perFn } = analyze(fns);
const hot = Array.from(fns.values())
  .sort((a, b) => (b.entries - a.entries) || (a.key < b.key ? -1 : a.key > b.key ? 1 : 0))
  .slice(0, top);

if (asJson) {
  const totalsObj = {};
  for (const k of Object.keys(total)) totalsObj[k] = total[k].asObject();
  const hotArr = hot.map(f => {
    const m = perFn.get(f.key);
    return {
      key: f.key,
      entries: f.entries,
      dyn_mono: m.dyn_mono.asObject(),
      dyn_num: m.dyn_num.asObject(),
      ic_mono: m.ic_mono.asObject(),
      call_mono: m.call_mono.asObject(),
    };
  });
  console.log(JSON.stringify({ functions: fns.size, totals: totalsObj, hot: hotArr }, null, 2));
  Bronze.exit(0);
}

console.log(`${profilePath}: ${fns.size} functions`);
const rows = [
  ['dynamic-op executions, one tag', total.dyn_mono],
  ['dynamic-op executions, numbers only', total.dyn_num],
  ['IC executions, one shape', total.ic_mono],
  ['call executions, one target', total.call_mono],
];
for (const [label, t] of rows) {
  console.log(
    `  ${label.padEnd(40)} ${pct(t.share())}  of ${fmtInt(t.execs).padStart(12)} execs` +
    `   (${fmtInt(t.siteHit)}/${fmtInt(t.sites)} sites)`
  );
}
console.log('');
console.log(`  ${'entries'.padStart(10)}  ${'1-tag'.padStart(6)}  ${'num'.padStart(6)}  ${'1-shape'.padStart(7)}  ${'1-tgt'.padStart(6)}  function`);
for (const f of hot) {
  const m = perFn.get(f.key);
  console.log(
    `  ${fmtInt(f.entries).padStart(10)}  ${pct(m.dyn_mono.share())}  ${pct(m.dyn_num.share())}` +
    `  ${pct(m.ic_mono.share()).padStart(7)}  ${pct(m.call_mono.share())}  ${f.key}`
  );
}

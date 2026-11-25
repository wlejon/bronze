// The oracle: every case under cases/ through the compiler, against its .expected,
// plus optional threejs and pixi real-library milestones.
//
// Compiled and operated by bronze itself (`bronze build tests/oracle/run.js -o bronze-oracle`):
//   bronze-oracle [filter ...] [--bronze PATH] [--cases-dir DIR]
//                 [--modes run,aot,profile,switch,adversarial,threejs,pixi]
//                 [--jobs N] [--work DIR]

import { Harvest, PROFILES, Rng, crc32, parseSlowSiteFunctions, splitLines } from './adversarial.js';

const EXE_EXT = Bronze.platform === 'windows' ? '.exe' : '';
const MARKER = 'const TRAINING = Math.random() < 0.5;';

// ---- CLI parsing ------------------------------------------------------------

const rawArgs = Bronze.args.slice();
function takeOption(name, fallback) {
  const idx = rawArgs.indexOf(name);
  if (idx >= 0) {
    if (idx + 1 >= rawArgs.length) {
      console.error(`missing value for ${name}`);
      Bronze.exit(2);
    }
    const val = rawArgs[idx + 1];
    rawArgs.splice(idx, 2);
    return val;
  }
  const prefix = name + '=';
  for (let i = 0; i < rawArgs.length; i++) {
    if (rawArgs[i].startsWith(prefix)) {
      const val = rawArgs[i].slice(prefix.length);
      rawArgs.splice(i, 1);
      return val;
    }
  }
  return fallback;
}

function detectOracleDir() {
  const explicit = takeOption('--cases-dir', '');
  if (explicit) return Bronze.path.resolve(explicit);
  const candidates = [
    Bronze.path.join(Bronze.cwd(), 'tests', 'oracle'),
    Bronze.cwd(),
    Bronze.path.join(Bronze.path.dirname(Bronze.exe), '..', '..', 'tests', 'oracle'),
  ];
  for (const c of candidates) {
    if (Bronze.fs.isDir(Bronze.path.join(c, 'cases'))) return Bronze.path.resolve(c);
  }
  return Bronze.path.resolve(Bronze.path.join(Bronze.cwd(), 'tests', 'oracle'));
}

function detectBronze(oracleDir) {
  const explicit = takeOption('--bronze', '');
  if (explicit) return Bronze.path.resolve(explicit);
  const exeDir = Bronze.path.dirname(Bronze.exe);
  const candidates = [
    Bronze.path.join(exeDir, '..', 'src', 'cli', 'bronze' + EXE_EXT),
    Bronze.path.join(exeDir, 'bronze' + EXE_EXT),
    Bronze.path.join(oracleDir, '..', '..', 'build', 'src', 'cli', 'bronze' + EXE_EXT),
  ];
  for (const c of candidates) {
    if (Bronze.fs.isFile(c)) return Bronze.path.resolve(c);
  }
  return Bronze.path.resolve(candidates[0]);
}

const oracleDir = detectOracleDir();
const bronzePath = detectBronze(oracleDir);
const modes = takeOption('--modes', 'run,aot,profile,switch,adversarial')
  .split(',')
  .map(s => s.trim())
  .filter(Boolean);
const envJobs = parseInt(Bronze.getenv('BRONZE_TEST_JOBS') || '', 10);
const defaultJobs = (envJobs > 0 ? envJobs : Math.min(Bronze.cpuCount(), 12));
const jobs = Math.max(1, parseInt(takeOption('--jobs', String(defaultJobs)), 10) || defaultJobs);
const work = Bronze.path.resolve(takeOption('--work', Bronze.path.join(Bronze.fs.tempDir(), 'bronze-oracle')));
const filters = rawArgs.slice();

const casesDir = Bronze.path.join(oracleDir, 'cases');
const caseModes = modes.filter(m => m !== 'threejs' && m !== 'pixi');
const runThreeJs = modes.includes('threejs');
const runPixi = modes.includes('pixi');

let names = [];
if (caseModes.length > 0) {
  const entries = Bronze.fs.listDir(casesDir);
  const expectedSet = new Set(entries.filter(f => f.endsWith('.expected')));
  const every = entries
    .filter(f => f.endsWith('.js') && expectedSet.has(f.slice(0, -3) + '.expected'))
    .map(f => f.slice(0, -3))
    .sort();
  const unmatched = filters.filter(f => !every.some(n => n.includes(f)));
  if (unmatched.length > 0) {
    console.log('no case under ' + casesDir + ' has a name containing: ' + unmatched.join(', '));
    Bronze.exit(2);
  }
  names = filters.length === 0 ? every : every.filter(n => filters.some(f => n.includes(f)));
}

// ---- Async process scheduler over Bronze.spawn + Bronze.waitAny -------------

const pendingProcs = new Map();

function sh(cmd, cwd, timeoutSec = 120, env = null) {
  return new Promise(resolve => {
    const opts = { cwd, timeoutMs: timeoutSec * 1000 };
    if (env) opts.env = env;
    try {
      const id = Bronze.spawn(cmd, opts);
      pendingProcs.set(id, resolve);
    } catch (e) {
      resolve({ status: -1, stdout: '', stderr: String(e), timedOut: false });
    }
  });
}

function differs(out, want) {
  const a = splitLines(out);
  const b = splitLines(want);
  const len = Math.max(a.length, b.length);
  for (let i = 0; i < len; i++) {
    const x = i < a.length ? a[i] : '<end>';
    const y = i < b.length ? b[i] : '<end>';
    if (x !== y) {
      return `line ${i + 1}: got ${JSON.stringify(x.slice(0, 70))} want ${JSON.stringify(y.slice(0, 70))}`;
    }
  }
  return null;
}

function lastNonEmptyLine(text, maxLen = 160) {
  const lines = splitLines(text).filter(l => l.trim().length > 0);
  return lines.length > 0 ? lines[lines.length - 1].slice(0, maxLen) : '';
}

function hasTrainingLine(stderr) {
  return splitLines(stderr).includes('TRAINING');
}

function isSwitching(srcPath) {
  return Bronze.fs.readText(srcPath).includes(MARKER);
}

async function runReal(cmd, cwd, switchingCase, env = null) {
  const attempts = switchingCase ? 40 : 1;
  let r = { status: -1, stdout: '', stderr: '', timedOut: false };
  for (let i = 0; i < attempts; i++) {
    r = await sh(cmd, cwd, 120, env);
    if (!hasTrainingLine(r.stderr)) break;
  }
  return r;
}

// ---- Case execution ---------------------------------------------------------

const advCounts = [];
const allFailed = [];

async function switchCase(name, srcPath, srcDir, d, tfb, buildFn, checkFn) {
  const hExe = Bronze.path.join(d, 'switch_h' + EXE_EXT);
  let res = await buildFn(['--harvest-out', tfb], hExe);
  if (!res.ok) return [[name, 'switch', 'harvest build: ' + res.why]];

  let trained = false;
  for (let i = 0; i < 40; i++) {
    Bronze.fs.unlink(tfb);
    const r = await sh([hExe], srcDir);
    if (hasTrainingLine(r.stderr) && Bronze.fs.exists(tfb)) {
      trained = true;
      break;
    }
  }
  if (!trained) return [[name, 'switch', 'no training run']];

  const fExe = Bronze.path.join(d, 'switch_f' + EXE_EXT);
  res = await buildFn(['--feedback-in', tfb], fExe);
  if (!res.ok) return [[name, 'switch', 'build: ' + res.why]];

  const r = await runReal([fExe], srcDir, true, { BRONZE_DEOPT_STATS: '1' });
  if (hasTrainingLine(r.stderr)) return [[name, 'switch', 'no run off the training arm']];
  Bronze.fs.writeText(Bronze.path.join(d, 'deopt.txt'), r.stderr);
  checkFn('switch', r);
  return [];
}

async function runAdversarial(name, srcPath, srcDir, switchingCase, d, tfb, want, wantRc) {
  const fails = [];
  let speculated = 0;
  let exits = 0;

  const ilRes = await sh([bronzePath, 'il', srcPath], d, 300);
  if (ilRes.status !== 0) {
    return { fails: [[name, 'adversarial', 'il: ' + lastNonEmptyLine(ilRes.stderr)]], speculated: 0, exits: 0 };
  }
  const functions = parseSlowSiteFunctions(ilRes.stdout);
  const tfbText = Bronze.fs.readText(tfb);

  for (let k = 0; k < PROFILES.length; k++) {
    const h = new Harvest(tfbText);
    h.functions = functions;
    const rng = new Rng(crc32(`${name}/${k}`));
    for (const p of PROFILES[k]) p(h, rng);

    const bad = Bronze.path.join(d, `adv${k}.tfb`);
    Bronze.fs.writeText(bad, h.text());
    const advExe = Bronze.path.join(d, `adv${k}${EXE_EXT}`);
    const bRes = await sh([bronzePath, 'build', srcPath, '-o', advExe, '--feedback-in', bad, '--opt-stats'], d, 300);
    if (bRes.status !== 0) {
      const tail = lastNonEmptyLine(bRes.stderr || bRes.stdout);
      fails.push([name, 'adversarial', `profile ${k}: build rc=${bRes.status}: ${tail}`]);
      continue;
    }
    const specMatch = /^speculate: (\d+)\//m.exec(bRes.stdout + bRes.stderr);
    if (specMatch && parseInt(specMatch[1], 10) > 0) speculated += 1;

    const r = await runReal([advExe], srcDir, switchingCase, { BRONZE_DEOPT_STATS: '1' });
    Bronze.fs.writeText(Bronze.path.join(d, `deopt${k}.txt`), r.stderr);
    const deoptRe = /^deopt: (\d+) exits/gm;
    let dm;
    while ((dm = deoptRe.exec(r.stderr)) !== null) exits += parseInt(dm[1], 10);

    let why = differs(r.stdout, want);
    if (why === null && r.status !== wantRc) {
      why = `exit status ${r.status}, the harvest run's ${wantRc}`;
    }
    if (why !== null) {
      const said = splitLines(r.stderr).filter(x => x.trim() && !x.startsWith('deopt:') && !x.startsWith(' '));
      const tail = said.length > 0 ? said[said.length - 1].slice(0, 120) : (r.timedOut ? 'timeout' : '');
      fails.push([name, 'adversarial', `profile ${k}: rc=${r.status} ${why} ${tail}`]);
    }
  }
  return { fails, speculated, exits };
}

async function runCase(name) {
  const srcPath = Bronze.path.join(casesDir, name + '.js');
  const expPath = Bronze.path.join(casesDir, name + '.expected');
  const want = Bronze.fs.readText(expPath);
  const switchingCase = isSwitching(srcPath);
  const d = Bronze.path.join(work, name);
  Bronze.fs.rm(d);
  Bronze.fs.mkdir(d);
  const tfb = Bronze.path.join(d, 'case.tfb');
  const fails = [];

  function check(mode, r) {
    const why = differs(r.stdout, want);
    if (why !== null) {
      const tail = r.timedOut ? 'timeout' : lastNonEmptyLine(r.stderr, 120);
      fails.push([name, mode, `rc=${r.status} ${why} ${tail}`]);
    }
  }

  async function build(extra, outExe) {
    const r = await sh([bronzePath, 'build', srcPath, '-o', outExe, ...extra], d, 300);
    return {
      ok: r.status === 0,
      why: r.status === 0 ? '' : (r.timedOut ? 'timeout' : lastNonEmptyLine(r.stderr || r.stdout)),
    };
  }

  if (caseModes.includes('run')) {
    check('run', await runReal([bronzePath, 'run', srcPath], casesDir, switchingCase));
  }
  if (caseModes.includes('aot')) {
    const aotExe = Bronze.path.join(d, 'aot' + EXE_EXT);
    const b = await build([], aotExe);
    if (b.ok) check('aot', await runReal([aotExe], casesDir, switchingCase));
    else fails.push([name, 'aot', 'build: ' + b.why]);
  }
  const fed = caseModes.filter(m => m === 'profile' || m === 'adversarial');
  if (fed.length > 0) {
    const harvestExe = Bronze.path.join(d, 'harvest' + EXE_EXT);
    const b = await build(['--harvest-out', tfb], harvestExe);
    const harvestRun = b.ok ? await sh([harvestExe], casesDir) : null;
    for (const mode of fed) {
      if (!b.ok) {
        fails.push([name, mode, 'harvest build: ' + b.why]);
      } else if (!Bronze.fs.exists(tfb)) {
        fails.push([name, mode, 'no harvest written']);
      } else if (mode === 'adversarial') {
        const adv = await runAdversarial(name, srcPath, casesDir, switchingCase, d, tfb, want, harvestRun.status);
        for (const f of adv.fails) fails.push(f);
        advCounts.push([adv.speculated, adv.exits]);
      } else {
        const profExe = Bronze.path.join(d, 'profile' + EXE_EXT);
        const pb = await build(['--feedback-in', tfb], profExe);
        if (pb.ok) check('profile', await runReal([profExe], casesDir, switchingCase));
        else fails.push([name, 'profile', 'build: ' + pb.why]);
      }
    }
  }
  if (caseModes.includes('switch') && switchingCase) {
    const sf = await switchCase(name, srcPath, casesDir, d, tfb, build, check);
    for (const f of sf) fails.push(f);
  }
  if (!fails.some(f => f[1] === 'switch' || f[1] === 'adversarial')) {
    Bronze.fs.rm(d);
  }
  return fails;
}

async function runMilestone(kind) {
  const dir = Bronze.path.join(oracleDir, kind);
  const srcPath = Bronze.path.join(dir, 'main.js');
  const expPath = Bronze.path.join(dir, 'main.expected');
  const want = Bronze.fs.readText(expPath);
  const d = Bronze.path.join(work, '_milestone_' + kind);
  Bronze.fs.rm(d);
  Bronze.fs.mkdir(d);
  const fails = [];

  function checkStep(label, r) {
    if (r.status !== 0) {
      fails.push([kind, kind, `${label}: rc=${r.status} ${r.timedOut ? 'timeout' : lastNonEmptyLine(r.stderr, 120)}`]);
      return;
    }
    const why = differs(r.stdout, want);
    if (why !== null) fails.push([kind, kind, `${label}: ${why}`]);
  }

  checkStep('run', await sh([bronzePath, 'run', srcPath], dir, 300));

  const aotExe = Bronze.path.join(d, `${kind}_aot${EXE_EXT}`);
  const bAot = await sh([bronzePath, 'build', srcPath, '-o', aotExe], d, 300);
  if (bAot.status !== 0) fails.push([kind, kind, 'aot build: ' + lastNonEmptyLine(bAot.stderr || bAot.stdout)]);
  else checkStep('aot', await sh([aotExe], dir, 300));

  const tfb = Bronze.path.join(d, `${kind}.tfb`);
  const hExe = Bronze.path.join(d, `${kind}_h${EXE_EXT}`);
  const bH = await sh([bronzePath, 'build', srcPath, '-o', hExe, '--harvest-out', tfb], d, 300);
  if (bH.status !== 0) {
    fails.push([kind, kind, 'harvest build: ' + lastNonEmptyLine(bH.stderr || bH.stdout)]);
  } else {
    const hRun = await sh([hExe], dir, 300);
    if (hRun.status !== 0 || !Bronze.fs.exists(tfb)) {
      fails.push([kind, kind, `harvest run failed (rc=${hRun.status})`]);
    } else {
      const pExe = Bronze.path.join(d, `${kind}_p${EXE_EXT}`);
      const bP = await sh([bronzePath, 'build', srcPath, '-o', pExe, '--feedback-in', tfb], d, 300);
      if (bP.status !== 0) fails.push([kind, kind, 'profile build: ' + lastNonEmptyLine(bP.stderr || bP.stdout)]);
      else checkStep('profile', await sh([pExe], dir, 300));
    }
  }

  if (fails.length === 0) Bronze.fs.rm(d);
  return fails;
}

// ---- Driver loop ------------------------------------------------------------

const tasks = [];
for (const name of names) tasks.push(() => runCase(name));
if (runThreeJs) tasks.push(() => runMilestone('threejs'));
if (runPixi) tasks.push(() => runMilestone('pixi'));

let nextIdx = 0;
let finished = false;

async function worker() {
  while (true) {
    const idx = nextIdx++;
    if (idx >= tasks.length) return;
    const fails = await tasks[idx]();
    for (const f of fails) allFailed.push(f);
  }
}

const workerCount = Math.min(jobs, Math.max(1, tasks.length));
const workers = [];
for (let i = 0; i < workerCount; i++) workers.push(worker());
Promise.all(workers).then(() => { finished = true; });
Bronze.drainMicrotasks();

while (!finished) {
  const ev = Bronze.waitAny();
  if (!ev) {
    Bronze.drainMicrotasks();
    if (!finished) break;
    continue;
  }
  const resolve = pendingProcs.get(ev.id);
  pendingProcs.delete(ev.id);
  if (resolve) resolve(ev);
  Bronze.drainMicrotasks();
}

allFailed.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : a[1] < b[1] ? -1 : a[1] > b[1] ? 1 : 0));
for (const [name, mode, why] of allFailed) {
  console.log(`FAIL ${mode.padEnd(8)} ${name}: ${why}`);
}

const switchCount = caseModes.includes('switch')
  ? names.filter(n => isSwitching(Bronze.path.join(casesDir, n + '.js'))).length
  : 0;

for (const mode of modes) {
  const bad = allFailed.filter(f => f[1] === mode).length;
  let total = names.length;
  if (mode === 'switch') total = switchCount;
  else if (mode === 'threejs' || mode === 'pixi') total = 1;
  console.log(`${mode}: ${total - bad}/${total} pass`);
}

if (advCounts.length > 0) {
  const specSum = advCounts.reduce((acc, x) => acc + x[0], 0);
  const exitSum = advCounts.reduce((acc, x) => acc + x[1], 0);
  console.log(`adversarial: ${specSum}/${advCounts.length * PROFILES.length} perturbed builds speculated, ${exitSum} guard exits`);
}

Bronze.exit(allFailed.length > 0 ? 1 : 0);

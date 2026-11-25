// Interleaved benchmark measurement protocol.
//
//   bronze run bench/tools/interleave.js -- <spec.json> [rounds]
//   # or compiled: bronze build bench/tools/interleave.js -o interleave

const LINE_RE = /^\[bench\] (\S+) ms=([0-9.]+)(?: ns_per_iter=([0-9.]+))?/gm;

function median(vals) {
  const s = vals.slice().sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 === 1 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
}

function runOnce(col) {
  const cmd = col.js ? ['node', col.js] : [col.exe];
  const p = Bronze.exec(cmd, { timeoutMs: 120000 });
  const ms = new Map();
  const per = new Map();
  LINE_RE.lastIndex = 0;
  let m;
  while ((m = LINE_RE.exec(p.stderr)) !== null) {
    ms.set(m[1], parseFloat(m[2]));
    if (m[3]) per.set(m[1], parseFloat(m[3]));
  }
  if (ms.size === 0) {
    console.error(
      `${col.name}: printed no '[bench] ... ms=' line.\n` +
      `  ran: ${cmd.join(' ')}\n` +
      `  stderr: ${p.stderr.trim().slice(0, 400)}\n` +
      'A fixture must time itself through bench/harness.js.'
    );
    Bronze.exit(1);
  }
  return { ms, per, stdout: p.stdout.trim() };
}

const args = Bronze.args;
if (args.length < 1) {
  console.error('usage: interleave <spec.json> [rounds]');
  Bronze.exit(2);
}

const spec = JSON.parse(Bronze.fs.readText(args[0]));
const rounds = args.length > 1 ? parseInt(args[1], 10) : 101;

const samples = new Map();
const pers = new Map();
const out = new Map();

for (let r = 0; r <= rounds; r++) {
  for (const col of spec) {
    const res = runOnce(col);
    if (r >= 1) {
      for (const [region, v] of res.ms.entries()) {
        const key = `${col.name}\0${region}`;
        if (!samples.has(key)) samples.set(key, []);
        samples.get(key).push(v);
      }
      for (const [region, v] of res.per.entries()) {
        const key = `${col.name}\0${region}`;
        if (!pers.has(key)) pers.set(key, []);
        pers.get(key).push(v);
      }
    }
    out.set(col.name, res.stdout);
  }
}

let width = 0;
for (const key of samples.keys()) {
  const [c, region] = key.split('\0');
  const labelLen = (c + '/' + region).length;
  if (labelLen > width) width = labelLen;
}
width += 2;

for (const col of spec) {
  const name = col.name;
  for (const [key, vals] of samples.entries()) {
    const [c, region] = key.split('\0');
    if (c !== name) continue;
    const label = region === name ? region : `${name}/${region}`;
    const pVals = pers.get(key);
    const tail = pVals
      ? `  ${median(pVals).toFixed(3).padStart(8)} ns/iter`
      : ' '.repeat(19);
    const checksum = (out.get(name) || '').replace(/\r?\n/g, ' | ');
    console.log(`${label.padEnd(width)} ${median(vals).toFixed(3).padStart(9)} ms${tail}   ${checksum}`);
  }
}

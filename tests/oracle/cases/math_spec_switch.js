// TRAINING true runs the warm-up alone: a harvest of that run, fed to a
// build of this file, makes every switched call below fail a guard.
const TRAINING = false;
// Math intrinsics trained on Numbers, then given other values and replaced.
function m1(x) { return [Math.floor(x), Math.ceil(x), Math.round(x), Math.sqrt(x), Math.abs(x), Math.sin(x), Math.exp(x), Math.log(x)]; }
function m2(x, y) { return [Math.min(x, y), Math.max(x, y), Math.imul(x, y), Math.atan2(x, y)]; }
function mix(a, b) { return Math.imul(a ^ b, 0x5bd1e995) >>> 0; }
function via(M, a, b) { return M.imul(a, b) + M.floor(a / b); }
let acc = 0;
for (let i = 0; i < 20000; i++) {
  const x = i * 0.37 + 0.5;
  const r = m1(x), q = m2(x, i % 17);
  acc += r[0] + r[1] + r[2] + r[3] + r[4] + r[5] + q[0] + q[1] + q[2] + q[3];
  acc = acc % 1e9;
  acc += mix(i, 12345) % 1000 + via(Math, i, 7);
}
console.log(acc.toFixed(6));
const show = (v) => Array.isArray(v) ? v.map(show).join(' ') : Object.is(v, -0) ? '-0' : String(v);
if (!TRAINING) {
  for (const x of [-0, 0, -0.4, -0.5, 0.5, 2.5, -2.5, NaN, Infinity, -Infinity, -4, 1e300, '9', null, undefined, true, { valueOf() { return 16; } }]) console.log(show(m1(x)));
  for (const [x, y] of [[0, -0], [-0, 0], [NaN, 1], [1, NaN], [2 ** 32 + 3, 5], [-1.9, 3.9], ['4', '5'], [null, 7], [0xffffffff, 0xffffffff]]) console.log(show(m2(x, y)));
  try { m1(1n); } catch (e) { console.log(e.constructor.name); }
  console.log(mix(1.5, 2), mix('7', 1), mix(2 ** 31, 2 ** 31));
  // Bronze freezes the Math namespace, so another receiver stands in for a
  // replaced member.
  const fake = { imul: (a, b) => a * b + 0.5, floor: (x) => 'F' + x };
  console.log(via(fake, 3, 4), via(Math, 3, 4), via({ __proto__: Math }, 7, 2), via({ imul: Math.floor, floor: Math.imul }, 9, 2));
  try { via({}, 1, 2); } catch (e) { console.log(e.constructor.name); }
  let reads = 0;
  const counted = { get imul() { reads++; return Math.imul; }, floor: Math.floor };
  console.log(via(counted, 6, 7), via(counted, 1.5, 2), reads);
}

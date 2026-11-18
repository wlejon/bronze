// A harness seeds Math.random and fixes Date.now so an animated scene draws the
// same frame twice. Both are replaceable; the rest of Math and Date stay as
// they were.
const origRandom = Math.random;
const origNow = Date.now;
console.log(typeof origRandom, typeof origNow, origRandom() < 1, origNow() > 1.5e12);

// Sites that ran, and cached what they read, before the replacement.
function warm() { return [Math.random(), Date.now(), Math.random, Date.now]; }
for (let i = 0; i < 2000; i++) warm();

let seed = 1;
function mulberry32() {
  seed |= 0; seed = seed + 0x6D2B79F5 | 0;
  let t = Math.imul(seed ^ seed >>> 15, 1 | seed);
  t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
  return ((t ^ t >>> 14) >>> 0) / 4294967296;
}
Math.random = mulberry32;
Date.now = () => 1000;
console.log(Math.random === mulberry32, Date.now === Date.now, Date.now());
seed = 1;
const w = warm();
console.log(w[0].toFixed(9), w[1], w[2] === mulberry32, w[3]());

function particles(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += Math.random() * 10 + Date.now();
  return sum.toFixed(6);
}
console.log(particles(1000));
seed = 1;
console.log(particles(1000));

// Through an alias, a call, an apply, a destructuring and a member read.
const { random } = Math;
seed = 1;
console.log(random().toFixed(9), Math.random.call(null).toFixed(9), Reflect.apply(Math.random, Math, []).toFixed(9));

// A replacement that is not a number-returning function.
Math.random = () => 'four';
console.log(Math.random(), typeof Math.random(), Math.random() + 1);
Date.now = 42;
console.log(Date.now, typeof Date.now);
try { Date.now(); } catch (e) { console.log(e.constructor.name); }

// Put back.
Math.random = origRandom;
Date.now = origNow;
console.log(Math.random === origRandom, Math.random() < 1, Date.now() > 1.5e12);

// A guard that fails in the middle of a function, after side effects the
// function has already made: an image's speculated code leaves for its
// resume twin (docs/aot-specialization.md, "Twin exits") with those effects
// done once, and the twin finishes the call from the failing read on.

class P { constructor(x, y) { this.x = x; this.y = y; } }
const log = [];
let calls = 0;

function step(a, b, i) {
  calls++;                       // an effect before any guard on `b`
  a.x += 1;                      // a store through the first receiver
  log.push(i & 3);               // a growing array
  const t = b.y * 2;             // the read that meets another shape
  a.y = t + a.x;
  return t + calls;
}

function run(n, odd) {
  const a = new P(0, 0);
  const b = new P(1, 2);
  let s = 0;
  for (let i = 0; i < n; i++) s += step(a, i % 997 === 996 ? odd : b, i);
  return s + ':' + a.x + ':' + a.y + ':' + calls + ':' + log.length;
}

console.log(run(20000, new P(3, 4)));
console.log(run(20000, { y: 5, extra: 1 }));
console.log(run(20000, { extra: 1, y: 7 }));
const d = { y: 9, gone: 1 };
delete d.gone;
console.log(run(20000, d));
console.log(run(3, { get y() { calls += 100; return 11; } }));

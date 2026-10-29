// Tier changes in the middle of a run keep a program's results: a closure
// first reached from compiled code while it is still in Tier 0 (in the
// tiered engine, through the function's stub, which compiles it or runs it
// behind a native-to-Tier-0 bridge that counts its calls), a recursion that
// crosses the tier-up thresholds inside one call, a function whose
// arguments change kind after it tiered up (a deopt) and that tiers up
// again, a property read whose receivers change shape after tier-up, and a
// loop that tiers up mid-run (OSR) while closures it made hold its
// bindings. Each round runs everything again in later tiers.

function apply(f, x) { return f(x); }

// `sq` warms `apply` up; `cube` is first called by compiled code.
function bridged() {
  const sq = (x) => x * x;
  let s = 0;
  for (let i = 0; i < 3000; i++) s += apply(sq, i % 10);
  const cube = (x) => x * x * x;
  let c = 0;
  for (let i = 0; i < 3000; i++) c += apply(cube, i % 10);
  return s + ' ' + c;
}

function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

class Tree {
  constructor(v) { this.v = v; this.kids = []; }
  sum() {
    let s = this.v;
    for (const k of this.kids) s += k.sum();
    return s;
  }
}

function treeSums() {
  let t = 0;
  for (let r = 0; r < 300; r++) {
    const root = new Tree(1);
    for (let i = 0; i < 10; i++) root.kids.push(new Tree(i));
    t += root.sum();
  }
  return t;
}

function add(a, b) { return a + b; }

function retier() {
  let a1 = 0;
  for (let i = 0; i < 2000; i++) a1 = add(a1, 1);
  const s1 = add('x', 1);
  let a2 = 0;
  for (let i = 0; i < 2000; i++) a2 = add(a2, 0.5);
  const big = add(2 ** 30, 2 ** 30);
  return [a1, s1, a2, big];
}

function getX(o) { return o.x; }

function shapes() {
  let t = 0;
  for (let i = 0; i < 2000; i++) t += getX({ x: i & 1 });
  t += getX({ y: 1, x: 5 });
  const missing = getX({});
  let u = 0;
  for (let i = 0; i < 2000; i++) u += getX(i % 2 ? { x: 1 } : { y: 0, x: 2 });
  return [t, String(missing), u];
}

function osr(n) {
  const fs = [];
  let acc = 0;
  for (let i = 0; i < n; i++) {
    const j = i;
    if (i % 1000 === 0) fs.push(() => j * 2 + acc % 7);
    acc += i & 3;
  }
  let t = 0;
  for (const f of fs) t += f();
  return [acc, fs.length, t];
}

for (let round = 0; round < 3; round++) {
  console.log('round', round);
  console.log('bridged', bridged());
  console.log('fib', fib(20));
  console.log('tree', treeSums());
  console.log('retier', retier().join(' '));
  console.log('shapes', shapes().join(' '));
  console.log('osr', osr(5000).join(' '));
}

// Every call site here runs hot on a callee compiled code may enter directly
// (bronze_abi.h, BRONZE_ABI_FN_FLAG_DIRECT_CALL) long enough for the tiered
// engine to optimize it, mid-loop and whole, and only then meets what the
// direct entry must refuse or re-test: a class constructor (whose call
// throws), a bound function, a Proxy, a native, a function reading
// new.target; a method replaced or shadowed after warm-up; a class getter
// redefined, and a subclass that overrides it; a constructor whose prototype
// is swapped; a callee that starts throwing through the direct entry. Each
// answer is checked after the break, then the site is driven hot again.

"use strict";
const N = 20000;

// A closure-call site.
function inc(x) { return x + 1; }
function readsTarget() { return new.target === undefined ? 7 : 8; }
class K { constructor(x) { this.x = x; } }
function callIt(f, x) { return f(x); }
function drive(f) {
  let s = 0;
  for (let i = 0; i < N; i++) s = callIt(f, s);
  return s;
}
const out = [];
out.push("warm " + drive(inc));
const breakers = [
  ["class", K],
  ["bound", inc.bind(null)],
  ["proxy", new Proxy(inc, { apply(t, self, args) { return t(args[0]) * 2; } })],
  ["native", Math.abs],
  ["target", readsTarget],
];
for (const [name, f] of breakers) {
  try {
    out.push(name + " " + callIt(f, -3));
  } catch (e) {
    out.push(name + " " + e.constructor.name);
  }
  out.push("rewarm " + drive(inc));
}

// A method-call site: replaced on the prototype, then shadowed on one object.
class Counter {
  constructor() { this.n = 0; }
  step() { this.n += 1; return this.n; }
}
function stepAll(cs) {
  let s = 0;
  for (let i = 0; i < N; i++) s += cs[i & 3].step();
  return s;
}
const cs = [new Counter(), new Counter(), new Counter(), new Counter()];
out.push("method " + stepAll(cs));
Counter.prototype.step = function () { this.n += 10; return -1; };
out.push("replaced " + stepAll(cs) + " " + cs[0].n);
cs[2].step = function () { return 1000; };
out.push("shadowed " + stepAll(cs) + " " + cs[2].n);

// An accessor site: the getter redefined after warm-up, then a subclass's
// override arrives at the same site.
class Box {
  constructor(v) { this.v = v; }
  get twice() { return this.v * 2; }
}
class ThriceBox extends Box {
  get twice() { return this.v * 3; }
}
function sumTwice(bs) {
  let s = 0;
  for (let i = 0; i < N; i++) s += bs[i & 1].twice;
  return s;
}
out.push("getter " + sumTwice([new Box(1), new Box(2)]));
Object.defineProperty(Box.prototype, "twice", { get() { return -this.v; }, configurable: true });
out.push("redefined " + sumTwice([new Box(1), new Box(2)]));
out.push("override " + sumTwice([new Box(1), new ThriceBox(2)]));

// A `new` site whose constructor's prototype is swapped after warm-up.
function Pt(x) { this.x = x; }
Pt.prototype.tag = function () { return "a"; };
function makeAll() {
  let last = null;
  let s = 0;
  for (let i = 0; i < N; i++) { last = new Pt(i); s += last.x; }
  return s + ":" + last.tag() + ":" + (last instanceof Pt);
}
out.push("new " + makeAll());
const old = Pt.prototype;
Pt.prototype = { tag() { return "b"; } };
out.push("swapped " + makeAll() + ":" + (Object.getPrototypeOf(new Pt(0)) === old));

// A callee that starts throwing once the site is hot: the throw leaves
// through the direct entry to the caller's handler.
let limit = Infinity;
function guarded(x) {
  if (x >= limit) throw new RangeError("at " + x);
  return x + 1;
}
function runGuarded() {
  try {
    return "ran " + drive(guarded);
  } catch (e) {
    return e.constructor.name + " " + e.message;
  }
}
out.push(runGuarded());
limit = 12345;
out.push(runGuarded());
limit = Infinity;
out.push(runGuarded());

for (const line of out) console.log(line);

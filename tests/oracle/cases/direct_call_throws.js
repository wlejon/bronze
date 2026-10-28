// Exceptions through calls that compiled code may make without the runtime
// in between (bronze_abi.h, BRONZE_ABI_FN_FLAG_DIRECT_CALL): a closure call,
// a prototype method, a method held in the receiver's own slot, and `new`.
// Each throws from the callee and is caught by a handler in the caller, two
// callers up, and in a function whose own call sits inside a `try`; the
// stack names the frames in order; `finally` runs; recursion through each
// form ends in a RangeError that the program survives and recovers from.

function frames(stack) {
  const m = stack.match(/at (?:new )?([A-Za-z_$][\w$.]*)/g) || [];
  return m.map(s => s.replace(/^at (new )?/, "").replace(/^.*\./, "")).slice(0, 3).join(">");
}

function boom(n) { if (n === 13) throw new Error("boom" + n); return n; }
class Thing {
  constructor(n) { if (n === 13) throw new TypeError("ctor" + n); this.n = n; }
  hit(n) { if (n === 13) throw new RangeError("hit" + n); return n; }
}
const holder = { f: boom };
const thing = new Thing(1);

function viaClosure(f, n) { return f(n); }
function viaMethod(o, n) { return o.hit(n); }
function viaSlot(o, n) { return o.f(n); }
function viaNew(C, n) { return new C(n).n; }
const forms = [
  ["closure", (n) => viaClosure(boom, n)],
  ["method", (n) => viaMethod(thing, n)],
  ["slot", (n) => viaSlot(holder, n)],
  ["new", (n) => viaNew(Thing, n)],
];

// The handler two frames up from the throwing callee.
function outer(run) {
  let sum = 0, caught = "";
  for (let i = 0; i < 40; i++) {
    try { sum += run(i % 20); } catch (e) { caught += e.name + ":" + e.message + ";"; }
  }
  return sum + " " + caught;
}
for (const [name, run] of forms) console.log(name, outer(run));

// The handler in the caller itself: its call is inside the try.
function guarded(f, o, C, n) {
  let s = "";
  try { s += f(n); } catch (e) { s += "c:" + e.message; }
  try { s += o.hit(n); } catch (e) { s += "|m:" + e.message; }
  try { s += o.self(n); } catch (e) { s += "|s:" + e.message; }
  try { s += new C(n).n; } catch (e) { s += "|n:" + e.message; } finally { s += "!"; }
  return s;
}
thing.self = Thing.prototype.hit;
let g = "";
for (let i = 0; i < 30; i++) { const r = guarded(boom, thing, Thing, i % 15); if (i % 15 === 13) g += r + "\n"; }
console.log(g.trim());

// The stack through each form names the callee, then its caller, then the
// caller's: no frame of the call's own machinery shows between them.
function stackOf(run) { try { run(13); } catch (e) { return frames(e.stack); } return "none"; }
function closureTop(n) { return viaClosure(boom, n); }
function methodTop(n) { return viaMethod(thing, n); }
function slotTop(n) { return viaSlot(holder, n); }
function newTop(n) { return viaNew(Thing, n); }
const tops = [closureTop, methodTop, slotTop, newTop];
for (let i = 0; i < 30; i++) for (const top of tops) top(i % 10);
for (const top of tops) console.log(top.name, stackOf(top));

// Unbounded recursion through each form: a RangeError, caught, and calls
// work again afterwards.
function recClosure(f, n) { return f(f, n + 1); }
const recObj = { rec(n) { return this.rec(n + 1); } };
const recSlot = { n: 0 }; recSlot.rec = function (n) { return this.rec(n + 1); };
function Rec(n) { this.child = new Rec(n + 1); }
const deep = [
  ["closure", () => recClosure(recClosure, 0)],
  ["method", () => recObj.rec(0)],
  ["slot", () => recSlot.rec(0)],
  ["new", () => new Rec(0)],
];
for (const [name, run] of deep) {
  let kind = "none";
  try { run(); } catch (e) { kind = e instanceof RangeError ? "RangeError" : e.constructor.name; }
  console.log(name, kind, viaClosure(boom, 5), viaMethod(thing, 6), viaSlot(holder, 7), viaNew(Thing, 8));
}

// An Array method entered from the method site (the EXOTIC entry) throws
// itself (reduce of an empty array), and calls back into a function that
// throws (map's callback): both reach the caller's handler.
function red(a) { return a.reduce((x, y) => x + y); }
function mapped(a, f) { return a.map(f).join(","); }
let natives = "";
for (let i = 0; i < 40; i++) {
  const a = i % 10 === 9 ? [] : [i, 1];
  try { natives += red(a) + " "; } catch (e) { natives += e.constructor.name + " "; }
  try {
    natives += mapped([i], (x) => { if (x === 13) throw new RangeError("cb" + x); return x * 2; }) + " ";
  } catch (e) { natives += e.message + " "; }
}
console.log(natives.trim());

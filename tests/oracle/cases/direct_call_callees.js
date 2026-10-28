// A call site in a loop meets every kind of callee: compiled functions it
// may enter directly (bronze_abi.h, BRONZE_ABI_FN_FLAG_DIRECT_CALL), among
// them ones with too few or too many arguments, rest and default parameters
// (which the callee fills itself); and each thing that must go through the
// runtime instead — a bound function, a Proxy, a native, a class constructor
// (which throws), a generator, an async function, a function reading
// new.target, and a non-callable. The same site sees them interleaved, so a
// fast path taken for one callee must re-test the next. Each callee
// allocates, for the GC-stress re-run.

"use strict";
function plain(a, b) { return "plain:" + a + "," + b; }
function arity(a, b, c) { return "arity:" + arguments.length + ":" + String(c); }
function rest(...r) { return "rest:" + r.length + ":" + r.join("/"); }
function defaults(a, b = "dflt") { return "defaults:" + a + "," + b; }
function readsTarget() { return "target:" + (new.target === undefined ? "none" : new.target.name); }
function readsThis() { return "this:" + (this === undefined ? "undef" : typeof this); }
const arrow = (a) => "arrow:" + a;
const bound = plain.bind(null, "B");
const proxy = new Proxy(plain, { apply(t, self, args) { return "proxy:" + t.apply(self, args); } });
class K { constructor(x) { this.x = x; } }
function* gen(a) { yield "gen:" + a; }
async function asy(a) { return a; }

const callees = [plain, arity, rest, defaults, readsTarget, readsThis, arrow, bound, proxy, Math.max, String, K, gen, asy, 42, null, undefined];

// A non-callable's message names the callee as each engine spells it, so
// only the class constructor's (whose wording the spec's hosts share) is shown.
function fail(e) { return e.constructor.name + (/Class constructor/.test(e.message) ? ":" + e.message : ""); }
function show(v) {
  if (v && typeof v.next === "function") return "iter:" + v.next().value;
  if (v instanceof Promise) return "promise";
  return String(v);
}

// One closure-call site.
function callIt(f, a, b) { return f(a, b); }
const out = [];
for (let round = 0; round < 3; round++) {
  for (let i = 0; i < callees.length; i++) {
    // The plain function between every other callee, so the site is warm on
    // a direct entry each time it meets the next.
    for (let w = 0; w < 20; w++) callIt(plain, w, round);
    try {
      out.push(show(callIt(callees[i], i, "r")));
    } catch (e) {
      out.push(fail(e));
    }
  }
}
for (let i = 0; i < callees.length; i++) console.log(out[i]);
console.log(out.slice(callees.length).join("|") === [...out.slice(0, callees.length), ...out.slice(0, callees.length)].join("|"));

// The same callees through a method site on one receiver shape (the SLOT
// form: the receiver's own property holds the function).
function viaSlot(o) { return o.m(o.k); }
const res = [];
for (let i = 0; i < callees.length; i++) {
  const o = { m: plain, k: i };
  for (let w = 0; w < 20; w++) viaSlot(o);
  o.m = callees[i];
  try { res.push(show(viaSlot(o))); } catch (e) { res.push(fail(e)); }
}
console.log(res.join("\n"));

// And through a prototype method replaced on the prototype between calls.
class Host { m(a) { return "proto:" + a; } }
const h = new Host();
function viaProto(o, a) { return o.m(a); }
const pres = [];
for (let i = 0; i < callees.length; i++) {
  Host.prototype.m = function (a) { return "proto:" + a; };
  for (let w = 0; w < 20; w++) viaProto(h, w);
  Host.prototype.m = callees[i];
  try { pres.push(show(viaProto(h, i))); } catch (e) { pres.push(fail(e)); }
}
console.log(pres.join("\n"));

// Arity: fewer and more arguments than parameters, through each form.
console.log(callIt(arity, 1), callIt(arity, 1, 2));
console.log(viaSlot({ m: arity, k: 7 }), viaProto({ m: arity }, 3));

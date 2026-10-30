// Every object-literal property is DEFINED on the new object
// (13.2.5.5 PropertyDefinitionEvaluation: CreateDataPropertyOrThrow), never
// assigned. So an accessor or a read-only data property of the same name on
// Object.prototype is not consulted: no setter runs, nothing refuses, and the
// key becomes an own, writable, enumerable, configurable data property.

const log = [];
function f(n) {
  log.push("f" + n);
  return n;
}
function flush() {
  const s = log.join(",");
  log.length = 0;
  return s;
}
function own(o, k) {
  const d = Object.getOwnPropertyDescriptor(o, k);
  if (d === undefined) return k + ":absent";
  if ("get" in d) return k + ":accessor";
  return k + ":" + String(d.value) + (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") +
    (d.configurable ? "c" : "-");
}

Object.defineProperty(Object.prototype, "q", {
  set(v) {
    log.push("setter q " + v);
  },
  get() {
    return "inherited q";
  },
  configurable: true,
});
Object.defineProperty(Object.prototype, "w", {
  value: "inherited w",
  writable: false,
  configurable: true,
});

// The plain colon form, between two others: evaluation order is source order
// and the setter is never among it.
const plain = { p: f(1), q: f(2), r: f(3) };
console.log("plain", flush(), Object.keys(plain).join(","), own(plain, "q"));

// Shorthand.
const q = 7;
const short = { q };
console.log("shorthand", flush(), own(short, "q"));

// Computed, from a variable and from a string literal.
const k = "q";
const computed = { [k]: f(4) };
console.log("computed", flush(), own(computed, "q"));
const computedLit = { ["q"]: f(5) };
console.log("computed literal", flush(), own(computedLit, "q"));

// A string-literal key is a name like any other.
const quoted = { "q": f(6) };
console.log("quoted", flush(), own(quoted, "q"));

// A method is a data property holding the function.
const method = { q() { return "m"; } };
console.log("method", flush(), typeof method.q, method.q(), Object.keys(method).join(","));

// A getter and a setter define an accessor of their own: the inherited setter
// does not hear a later write, the literal's own one does.
const acc = {
  get q() { return "own getter"; },
  set q(v) { log.push("own setter " + v); },
};
acc.q = 1;
console.log("accessor", flush(), acc.q, own(acc, "q"));

// A non-writable inherited property refuses an assignment but not a
// definition, in every spelling.
const ro = { w: 1 };
const roShort = (() => { const w = 2; return { w }; })();
const roComputed = { ["w"]: 3 };
const roMethod = { w() { return 4; } };
console.log("read-only", own(ro, "w"), own(roShort, "w"), own(roComputed, "w"), roMethod.w());
// The assignment, for contrast: sloppy code discards it.
const assigned = {};
assigned.w = 5;
console.log("read-only assignment", own(assigned, "w"), assigned.w);
assigned.q = 6;
console.log("setter assignment", flush(), own(assigned, "q"));

// The same literals in strict code, where a refused assignment would throw.
(function () {
  "use strict";
  const s = { q: f(8), w: f(9) };
  console.log("strict", flush(), own(s, "q"), own(s, "w"));
  try {
    const t = {};
    t.w = 1;
    console.log("strict assignment stored");
  } catch (e) {
    console.log("strict assignment", e instanceof TypeError);
  }
})();

// Duplicate keys: the later definition wins, whatever kind either is.
const dupDataAccessor = { q: 1, get q() { return "later getter"; } };
console.log("data then accessor", own(dupDataAccessor, "q"), dupDataAccessor.q);
const dupAccessorData = { get q() { return "earlier getter"; }, q: 2 };
console.log("accessor then data", own(dupAccessorData, "q"));
const dupData = { q: 1, w: 2, q: 3 };
console.log("data twice", flush(), Object.keys(dupData).join(","), own(dupData, "q"));
const dupSetterGetter = { set q(v) { log.push("lit set " + v); }, q: 4, get w() { return 5; }, w: 6 };
console.log("setter then data", flush(), own(dupSetterGetter, "q"), own(dupSetterGetter, "w"));

// Spread copies by definition too, and reads the source with [[Get]]: a
// getter on the source runs, and its answer is stored as data.
const src = {};
Object.defineProperty(src, "q", { get() { log.push("src getter"); return "from getter"; }, enumerable: true });
Object.defineProperty(src, "w", { value: "src w", enumerable: true });
const spread = { ...src };
console.log("spread", flush(), own(spread, "q"), own(spread, "w"));
const spreadThenProp = { ...{ q: 1 }, q: 2 };
console.log("spread then prop", flush(), own(spreadThenProp, "q"));

// A rest pattern builds its object the same way.
const { p: _p, ...rest } = { p: 0, q: 10, w: 11 };
console.log("rest", flush(), own(rest, "q"), own(rest, "w"));

// Object.assign is the one copy that ASSIGNS (20.1.2.1 is Set): the setter
// runs, and the read-only property refuses with a TypeError.
const target = {};
Object.assign(target, { q: 12 });
console.log("assign", flush(), own(target, "q"));
try {
  Object.assign({}, { w: 13 });
  console.log("assign read-only stored");
} catch (e) {
  console.log("assign read-only", e instanceof TypeError);
}

// Object.fromEntries defines (CreateDataPropertyOrThrow).
const fromEntries = Object.fromEntries([["q", 14], ["w", 15]]);
console.log("fromEntries", flush(), own(fromEntries, "q"), own(fromEntries, "w"));

delete Object.prototype.q;
delete Object.prototype.w;
console.log("restored", ({}).q, ({}).w);

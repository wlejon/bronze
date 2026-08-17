// WeakRef (ECMA-262 26.1) and FinalizationRegistry (26.2): everything about
// them that is a FACT rather than a timing.
//
// The line this case does not cross is the whole reason it is short. Every
// oracle case runs four times — {infer, --no-infer} x {plain, GC stress} — and
// under stress the collector runs on every allocation while a plain run may
// never collect at all. So "does `deref` answer undefined after the last
// reference is dropped" cannot be pinned here: both answers are correct, and
// which one appears is the regime. The specification is even more permissive
// about cleanup callbacks, which a host may never call. Those two behaviours are
// pinned in tests/runtime/weak_ref_test.cpp, where C++ drives the collector
// directly and can assert exactly which weak slots were cleared.
//
// What IS pinnable is everything below, derived from ECMA-262:
//
// 1. 26.1.3.2 `deref` answers the target while something else holds it, and
//    9.13's KeepDuringJob makes two derefs in one job answer the same object
//    even if a collection lands between them.
// 2. 26.1.1.1 step 2 and 26.2.3.1 step 5 are TypeErrors for a value that
//    CANNOT be held weakly — a primitive, or a registered symbol, because
//    either can be re-created and so can never become unreachable.
// 3. 26.2.1.1 step 2 refuses a non-callable cleanup callback at construction.
// 4. 26.2.3.1 step 4 refuses `register(t, t)`: the held value is retained
//    strongly, so a target registered as its own held value could never fire.
// 5. 26.2.3.1 answers undefined; 26.2.3.2 answers whether a cell was removed,
//    so `unregister` of a held token is true once and false after.
// 6. 26.1.3.3 and 26.2.3.3 put @@toStringTag on the two prototypes, which is
//    the only reason `Object.prototype.toString` names them: 20.1.3.6's
//    builtin-tag list has no entry for either.
// 7. Neither has an own property (target and cells are internal slots), so
//    `Object.keys` is empty and `JSON.stringify` is `{}`.
// 8. An unregistered symbol CAN be held weakly (4.2.1), so `new WeakRef(sym)`
//    is legal and derefs to the symbol.
console.log(typeof WeakRef, typeof FinalizationRegistry);

const target = { tag: "kept" };
const wr = new WeakRef(target);
console.log(wr.deref() === target, wr.deref().tag);
// KeepDuringJob: whatever the collector did between these two reads, one job
// cannot see the target twice with different answers.
console.log(wr.deref() === wr.deref());

console.log(typeof wr.deref);
console.log("deref" in wr, "register" in wr, "hasOwnProperty" in wr);
console.log(Object.prototype.toString.call(wr));
console.log(wr[Symbol.toStringTag]);
console.log(Object.keys(wr).length, JSON.stringify({ w: wr }));
console.log(wr.hasOwnProperty("deref"));

const sym = Symbol("weak");
console.log(new WeakRef(sym).deref() === sym);

// A primitive and a registered symbol are the two things 4.2.1 excludes.
try {
  new WeakRef(1);
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}
try {
  new WeakRef(Symbol.for("registered"));
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}

const registry = new FinalizationRegistry(function () {});
console.log(Object.prototype.toString.call(registry));
console.log(registry[Symbol.toStringTag]);
console.log(typeof registry.register, typeof registry.unregister);
console.log("register" in registry, "unregister" in registry, "deref" in registry);
console.log(Object.keys(registry).length, JSON.stringify(registry));

// A token held for the whole program: the cell is there to be unregistered, so
// the first answer is true and the second is false, in every regime.
const token = {};
console.log(registry.register(target, "held", token));
console.log(registry.unregister(token), registry.unregister(token));

// A registration with no token is legal and simply cannot be unregistered.
console.log(registry.register(target, "no-token"));

try {
  new FinalizationRegistry(1);
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}
try {
  registry.register(target, target);
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}
try {
  registry.register(1, "held");
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}
try {
  registry.register(target, "held", 1);
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}

// A detached method has no receiver, and the brand check says so rather than
// reading some other object's bytes as a weak slot.
const detached = wr.deref;
try {
  detached();
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}
try {
  registry.unregister.call(wr, token);
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}

// console.log withholds the target for the reason a WeakMap's entries are
// withheld, and more sharply: printing it would be an observation of liveness
// that could differ from run to run.
console.log(wr);
console.log(registry);

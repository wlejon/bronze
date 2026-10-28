// Lockdown (runtime/lockdown.h): every intrinsic the realms of a thread share
// is frozen, an assignment that only inherits a frozen intrinsic's property
// still lands on the receiver (the override mistake), and the frozen objects
// stay as fast to read as before. Each case runs on a thread of its own,
// because a thread's intrinsics are its own and lockdown is forever: the
// other cases of this binary must keep the mutable builtins they test.
#include <doctest/doctest.h>

#include "embed/embed.h"
#include "eval/eval.h"
#include "runtime/gc.h"

#include <functional>
#include <string>
#include <thread>

using namespace bronze;
using namespace bronze::eval;

namespace {

// `source` evaluates to "ok" or to a description of what went wrong. `lock`
// false runs it on a thread of its own WITHOUT lockdown, for the intrinsics a
// plain program has.
std::string runLockedDown(const char* source, ExecutionTier tier = ExecutionTier::Auto,
                          const std::function<void()>& beforeLockdown = nullptr,
                          bool lock = true) {
    std::string out;
    std::thread worker([&] {
        ShadowStackFrame frame;
        if (beforeLockdown) beforeLockdown();
        if (lock) embed::lockdown();
        if (lock && !embed::lockedDown()) {
            out = "lockedDown() is false after lockdown()";
            return;
        }
        EvalOptions opts;
        opts.filename = "<lockdown>";
        opts.tier = tier;
        embed::CallResult r = evalScript(source, opts);
        if (r.thrown) {
            out = "threw: " + embed::toUtf8(r.value);
            return;
        }
        out = embed::isString(r.value) ? embed::toUtf8(r.value) : "not a string";
    });
    worker.join();
    return out;
}

// Every object reachable from the builtins and the hidden intrinsics, the walk
// lockdown makes, reported by a program: what is not frozen, and how many.
constexpr const char* kEverythingFrozen = R"JS(
const seen = new Set(), work = [];
const push = v => {
  if ((typeof v === "object" || typeof v === "function") && v !== null && !seen.has(v)) {
    seen.add(v); work.push(v);
  }
};
for (const n of ["Math", "Object", "Number", "JSON", "Array", "String", "Boolean", "Symbol",
  "BigInt", "RegExp", "Promise", "Map", "Set", "WeakMap", "WeakSet", "Error", "TypeError",
  "AggregateError", "RangeError", "SyntaxError", "ReferenceError", "URIError", "ArrayBuffer",
  "DataView", "Float32Array", "SharedArrayBuffer", "Atomics", "Function", "Proxy", "Reflect",
  "Date", "Intl", "Iterator", "WeakRef", "FinalizationRegistry", "eval", "parseInt"]) {
  push(globalThis[n]);
}
const P = Object.getPrototypeOf;
push(P(function* () {}));
push(P(async function () {}));
push(P(async function* () {}));
push(P([][Symbol.iterator]()));
push(P(new Map().entries()));
push(P(new Set().values()));
push(P(""[Symbol.iterator]()));
push(P("a".matchAll(/a/g)));
push(P([].values().map(x => x)));
push(P(Iterator.from({ next() { return { done: true }; } })));
while (work.length) {
  const o = work.pop();
  for (const k of Reflect.ownKeys(o)) {
    const d = Reflect.getOwnPropertyDescriptor(o, k);
    if ("value" in d) push(d.value); else { push(d.get); push(d.set); }
  }
  push(P(o));
}
const loose = [];
for (const o of seen) if (!Object.isFrozen(o)) loose.push(String(o.name || o[Symbol.toStringTag] || typeof o));
loose.length ? "not frozen: " + loose.join(", ") : seen.size > 900 ? "ok" : "only " + seen.size + " reached";
)JS";

}  // namespace

TEST_CASE("lockdown freezes every intrinsic reachable from the builtins") {
    CHECK(runLockedDown(kEverythingFrozen) == "ok");
}

TEST_CASE("a hardened intrinsic refuses every change") {
    const char* src = R"JS(
"use strict";
const bad = [];
const refuses = (what, f) => { try { f(); bad.push(what); } catch (e) { if (!(e instanceof TypeError)) bad.push(what + ": " + e); } };
refuses("add to Object.prototype", () => { Object.prototype.polluted = 1; });
refuses("replace Array.prototype.map", () => { Array.prototype.map = () => 1; });
refuses("add to Math", () => { Math.tau = 6.28; });
refuses("delete Math.max", () => { delete Math.max; });
refuses("define on Function.prototype", () => Object.defineProperty(Function.prototype, "q", { value: 1 }));
refuses("swap Object.prototype's prototype", () => Object.setPrototypeOf(Object.prototype, {}));
refuses("Error.stackTraceLimit", () => { Error.stackTraceLimit = 50; });
refuses("Promise.prototype.then", () => { Promise.prototype.then = null; });
refuses("JSON.parse", () => { JSON.parse = null; });
refuses("a static of Map", () => { Map.groupBy = null; });
refuses("the generator prototype", () => { Object.getPrototypeOf(function* () {}).prototype.next = null; });
refuses("the array iterator", () => { Object.getPrototypeOf([][Symbol.iterator]()).next = null; });
refuses("Reflect.ownKeys", () => { Reflect.ownKeys = null; });
refuses("a builtin function's own property", () => { Array.prototype.push.marker = 1; });
if (Reflect.set(Object.prototype, "x", 1)) bad.push("Reflect.set on Object.prototype answered true");
if (Object.isExtensible(Object.prototype)) bad.push("Object.prototype is extensible");
if (Error.stackTraceLimit !== 10) bad.push("stackTraceLimit " + Error.stackTraceLimit);
bad.length ? bad.join("; ") : "ok";
)JS";
    CHECK(runLockedDown(src) == "ok");
}

TEST_CASE("an assignment that only inherits a hardened property lands on the receiver") {
    const char* src = R"JS(
"use strict";
const bad = [];
const expect = (what, got, want) => { if (got !== want) bad.push(what + ": " + got); };
class MyError extends Error {}
MyError.prototype.name = "MyError";
expect("subclass name", new MyError("m").name, "MyError");
expect("subclass string", String(new MyError("m")), "MyError: m");
expect("Error.prototype.name untouched", Error.prototype.name, "Error");
const o = {};
o.toString = () => "mine";
expect("own toString", String(o), "mine");
expect("Object.prototype.toString untouched", Object.prototype.toString.call(o), "[object Object]");
function F() {}
F.prototype.toString = function () { return "F"; };
F.prototype.constructor = F;
expect("prototype toString", String(new F()), "F");
const fn = function () {};
fn.toString = () => "fn";
expect("function own toString", fn.toString(), "fn");
const r = {};
expect("Reflect.set answers true", Reflect.set(r, "valueOf", () => 7), true);
expect("Reflect.set landed", +r, 7);
const assigned = Object.assign({}, { toString: () => "assigned" });
expect("Object.assign", String(assigned), "assigned");
class Base { toString() { return "base"; } }
class Derived extends Base { constructor() { super(); this.toString = () => "derived"; } }
expect("this.toString in a constructor", String(new Derived()), "derived");
const desc = Object.getOwnPropertyDescriptor(Object.prototype, "toString");
expect("the intrinsic reports a frozen data property", desc.writable === false && desc.configurable === false && typeof desc.value, "function");
let sloppyRefused = (function () { try { Object.prototype.toString = null; return false; } catch (e) { return e instanceof TypeError; } })();
expect("the intrinsic itself still refuses", sloppyRefused, true);
bad.length ? bad.join("; ") : "ok";
)JS";
    CHECK(runLockedDown(src) == "ok");
    CHECK(runLockedDown(src, ExecutionTier::Tier0_Interpreter) == "ok");
    CHECK(runLockedDown(src, ExecutionTier::Tier2_Optimized) == "ok");
}

TEST_CASE("the global object stays the realm's own and writable") {
    const char* src = R"JS(
const bad = [];
globalThis.Array = 5;
if (globalThis.Array !== 5) bad.push("globalThis.Array");
globalThis.fresh = 1;
if (fresh !== 1) bad.push("fresh");
if (Object.isFrozen(globalThis)) bad.push("globalThis frozen");
bad.length ? bad.join("; ") : "ok";
)JS";
    CHECK(runLockedDown(src) == "ok");
}

TEST_CASE("two realms on one thread share the frozen intrinsics and nothing else") {
    std::string out = "did not run";
    std::thread worker([&] {
        ShadowStackFrame frame;
        embed::lockdown();
        embed::Realm* a = embed::createRealm();
        embed::Realm* b = embed::createRealm();
        EvalOptions opts;
        opts.filename = "<lockdown-realms>";
        std::string first, second;
        {
            embed::RealmScope scope(a);
            embed::CallResult r = evalScript(
                "Map.prototype.get = null; Object.prototype.leak = 1; globalThis.mine = 'a';"
                "class E extends Error {} E.prototype.name = 'E';"
                "typeof new Map().get + ',' + ({}).leak + ',' + new E().name",
                opts);
            first = r.thrown ? "threw: " + embed::toUtf8(r.value) : embed::toUtf8(r.value);
        }
        {
            embed::RealmScope scope(b);
            embed::CallResult r = evalScript(
                "typeof new Map().get + ',' + ({}).leak + ',' + typeof globalThis.mine + ',' + "
                "Error.prototype.name",
                opts);
            second = r.thrown ? "threw: " + embed::toUtf8(r.value) : embed::toUtf8(r.value);
        }
        embed::destroyRealm(a);
        embed::destroyRealm(b);
        out = first + "|" + second;
    });
    worker.join();
    CHECK(out == "function,undefined,E|function,undefined,undefined,Error");
}

TEST_CASE("reads of frozen intrinsics hit the caches in hot code") {
    // Method calls on built-in prototypes, a negative lookup that walks to
    // Object.prototype, statics and a frozen user object, run long enough to
    // tier up: the answers must not change once the caches name the frozen
    // shapes.
    const char* src = R"JS(
const frozen = Object.freeze({ a: 2, b: 3 });
class Point { constructor(x) { this.x = x; } }
let s = 0, misses = 0;
for (let i = 0; i < 200000; i++) {
  const p = new Point(i);
  if (p.hasOwnProperty("x")) s += 1;
  if (p.isMesh === undefined) misses += 1;
  s += frozen.a + frozen["b"];
  s += Math.max(1, 2) + Number.isInteger(i);
  s += String(i & 7).length;
}
s === 200000 * (1 + 5 + 2 + 1 + 1) && misses === 200000 ? "ok" : "s=" + s + " misses=" + misses;
)JS";
    CHECK(runLockedDown(src) == "ok");
    CHECK(runLockedDown(src, ExecutionTier::Tier2_Optimized) == "ok");
}

TEST_CASE("lockdown after a program patched a builtin freezes the patch in place") {
    const char* src = R"JS(
"use strict";
let refused = false;
try { Array.prototype.extra = 2; } catch (e) { refused = e instanceof TypeError; }
typeof Object.prototype.early === "function" && Object.isFrozen(Object.prototype.early) && refused ? "ok" : "not frozen";
)JS";
    CHECK(runLockedDown(src, ExecutionTier::Auto, [] {
              EvalOptions opts;
              opts.filename = "<before-lockdown>";
              evalScript("Object.prototype.early = function () {};", opts);
          }) == "ok");
}

// `Array.prototype` is the one intrinsic no array's chain runs through: an
// array answers its members from a table beside the value (rt_prop.cpp), so a
// property put on the object would be read off it and by nothing an array
// does. Unfrozen, every spelling of a change is therefore refused by name —
// including `Reflect.defineProperty` and `Reflect.set`, whose `false` would
// claim the object declined. Frozen, the object declines for itself: the
// oracle case `array_prototype_frozen_define` pins that half against node.
TEST_CASE("an unfrozen Array.prototype refuses every change by name") {
    const char* src = R"JS(
const bad = [];
const refuses = (what, f) => {
  try { f(); bad.push(what + " was accepted"); }
  catch (e) { if (!(e instanceof TypeError) || !/Cannot decorate Array\.prototype/.test(e.message)) bad.push(what + ": " + e); }
};
refuses("assign", () => { Array.prototype.push = () => 1; });
refuses("computed assign", () => { const k = "pu" + "sh"; Array.prototype[k] = 1; });
refuses("an index", () => { Array.prototype[0] = 1; });
refuses("a symbol", () => { Array.prototype[Symbol.iterator] = null; });
refuses("defineProperty", () => Object.defineProperty(Array.prototype, "shift", { value() { return 7; } }));
refuses("defineProperties", () => Object.defineProperties(Array.prototype, { pop: { value: 5 } }));
refuses("an accessor", () => Object.defineProperty(Array.prototype, "qq", { get() { return 4; } }));
refuses("Reflect.defineProperty", () => Reflect.defineProperty(Array.prototype, "slice", { value: 3 }));
refuses("Reflect.set", () => Reflect.set(Array.prototype, "shift", 1));
refuses("Reflect.set as receiver", () => Reflect.set({}, "shift", 1, Array.prototype));
refuses("super.k = v", () => ({ m() { super.zz = 1; } }).m.call(Array.prototype));
refuses("Object.assign", () => Object.assign(Array.prototype, { map() { return 1; } }));
refuses("delete", () => { delete Array.prototype.shift; });
refuses("Reflect.deleteProperty", () => Reflect.deleteProperty(Array.prototype, "map"));
refuses("Object.setPrototypeOf", () => Object.setPrototypeOf(Array.prototype, {}));
refuses("__proto__", () => { Array.prototype.__proto__ = {}; });
refuses("a static block", () => { class C { static { Object.defineProperty(Array.prototype, "x", { value: 1 }); } } });
if (delete Array.prototype.nope !== true) bad.push("deleting an absent key is no change, and true");
if (Object.setPrototypeOf(Array.prototype, Object.prototype) !== Array.prototype) bad.push("the same prototype");
if ([1, 2].shift() !== 1 || typeof Array.prototype.shift !== "function") bad.push("shift changed");
if (Object.getOwnPropertyNames(Array.prototype).includes("qq")) bad.push("qq landed");
bad.length ? bad.join("; ") : "ok";
)JS";
    for (ExecutionTier tier : {ExecutionTier::Auto, ExecutionTier::Tier0_Interpreter,
                               ExecutionTier::Tier2_Optimized}) {
        CHECK(runLockedDown(src, tier, nullptr, /*lock=*/false) == "ok");
    }
    // Under lockdown it is frozen like every intrinsic, and the answers are
    // the specification's.
    const char* locked = R"JS(
const bad = [];
try { Object.defineProperty(Array.prototype, "shift", { value() { return 7; } }); bad.push("defined"); }
catch (e) { if (!(e instanceof TypeError) || /decorate/.test(e.message)) bad.push("defineProperty: " + e); }
if (Reflect.defineProperty(Array.prototype, "shift", { value: 1 }) !== false) bad.push("Reflect.defineProperty");
if (Reflect.defineProperty(Array.prototype, "fresh", { value: 1 }) !== false) bad.push("Reflect.defineProperty new");
if (Reflect.set(Array.prototype, "shift", 1) !== false) bad.push("Reflect.set");
if (Reflect.deleteProperty(Array.prototype, "shift") !== false) bad.push("Reflect.deleteProperty");
if ([1].shift() !== 1) bad.push("shift");
bad.length ? bad.join("; ") : "ok";
)JS";
    CHECK(runLockedDown(locked) == "ok");
}

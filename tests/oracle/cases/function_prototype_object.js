// `Function.prototype`'s REFLECTIVE surface (ECMA-262 20.2.3).
//
// `call`, `apply`, `bind`, `toString` and `constructor` used to be a ladder of
// names answered beside a function receiver: a read found them, but they were
// nobody's own properties — `Object.getOwnPropertyNames(Function.prototype)`
// listed neither them nor `[Symbol.hasInstance]`, `hasOwnProperty("call")` was
// false, and a descriptor for `bind` was `undefined`. Now they are own
// non-enumerable data properties of the one `Function.prototype` object, with
// 20.2.3's name and length each, so every reflective question answers as the
// clause says while `f.call(...)` reads exactly what it always did. Annex B's
// `arguments` and `caller` (20.2.4 / B.2.2) are left out of the listings.

const FP = Function.prototype;
const legacy = ["arguments", "caller"];
const names = (o) =>
  Object.getOwnPropertyNames(o).filter((n) => !legacy.includes(n)).sort().join(",");
const desc = (o, k) => {
  const d = Object.getOwnPropertyDescriptor(o, k);
  return d === undefined
    ? "absent"
    : (d.get ? "get:" + d.get.name + "/set:" + typeof d.set : "value:" + typeof d.value) +
        " w:" + d.writable + " e:" + d.enumerable + " c:" + d.configurable;
};
const fails = (f) => {
  try {
    f();
    return "no throw";
  } catch (e) {
    return (e instanceof TypeError) + " " + e.name;
  }
};

// --- the object itself --------------------------------------------------
console.log("callable:", typeof FP, FP(), FP(1, 2), FP.length, JSON.stringify(FP.name));
console.log("chain:", Object.getPrototypeOf(FP) === Object.prototype,
  Object.getPrototypeOf(Function) === FP, FP.constructor === Function, Function.prototype === FP);
console.log("own names:", names(FP));
console.log("own symbols:", Object.getOwnPropertySymbols(FP).map((s) => s.description).join(","));
console.log("Reflect.ownKeys:",
  Reflect.ownKeys(FP).filter((k) => !legacy.includes(k)).map(String).sort().join(","));
console.log("hasOwn:", FP.hasOwnProperty("call"), Object.hasOwn(FP, "bind"), "apply" in FP,
  FP.hasOwnProperty("prototype"), "prototype" in FP, FP.prototype);
console.log("length/name descs:", desc(FP, "length"), desc(FP, "name"));

// --- descriptors, names and lengths of the members ----------------------
for (const k of ["apply", "bind", "call", "toString", "constructor"]) {
  console.log(k + ":", desc(FP, k), FP[k].name, FP[k].length);
}
console.log("hasInstance:", desc(FP, Symbol.hasInstance), FP[Symbol.hasInstance].name,
  FP[Symbol.hasInstance].length);
console.log("descriptor value identity:",
  Object.getOwnPropertyDescriptor(FP, "call").value === FP.call,
  Object.getOwnPropertyDescriptor(FP, "constructor").value === Function);

// --- inherited by every function, and one object each -------------------
function f(a, b) { return this.x + a + b; }
const arrow = () => 1;
class K { static s() {} m() {} }
console.log("inherited:", f.call === FP.call, arrow.apply === FP.apply, K.bind === FP.bind,
  K.prototype.m.toString === FP.toString, f.constructor === Function, K.s.constructor === Function);
console.log("not own:", f.hasOwnProperty("call"), Object.hasOwn(K, "apply"), "call" in f,
  Object.getOwnPropertyDescriptor(f, "bind"));
console.log("names of f:", names(f), names(arrow), names(K), names(K.s));

// --- the reflective calls ----------------------------------------------
console.log("call.call:", FP.call.call(f, { x: 1 }, 2, 3));
console.log("apply.call:", FP.apply.call(f, { x: 10 }, [2, 3]));
console.log("bind.call:", FP.bind.call(f, { x: 100 }, 1)(2));
console.log("Reflect.apply:", Reflect.apply(FP.call, f, [{ x: 5 }, 1, 1]));
const call = FP.call;
console.log("detached call:", call.call(f, { x: 7 }, 0, 0));
console.log("hasInstance.call:", FP[Symbol.hasInstance].call(K, new K()),
  FP[Symbol.hasInstance].call(K, {}), FP[Symbol.hasInstance].call(f, 1));

// --- wrong receivers ----------------------------------------------------
console.log("wrong receivers:", fails(() => FP.call.call({}, 1)),
  fails(() => FP.apply.call("s", null, [])), fails(() => FP.bind.call(1)),
  fails(() => FP.toString.call({})), fails(() => FP[Symbol.hasInstance].call(1, {})));

// --- shadowing and overriding stays ordinary ---------------------------
class Shadow { static call() { return "static call"; } }
console.log("static shadows:", Shadow.call(), Shadow.apply === FP.apply);
const own = function () {};
own.bind = () => "own bind";
console.log("own shadows:", own.bind(), Object.hasOwn(own, "bind"), own.call === FP.call);
const saved = FP.toString;
FP.toString = function () { return "patched " + typeof this; };
console.log("patched:", f.toString(), K.toString());
FP.toString = saved;
console.log("restored:", f.toString === saved, FP.toString === saved);
console.log("expando on FP:", (FP.extra = 3, f.extra), delete FP.extra, f.extra);

// --- toString on the intrinsics themselves ------------------------------
console.log("native forms:", FP.call.toString(), FP.toString.call(FP));
console.log("tag:", Object.prototype.toString.call(FP), Object.prototype.toString.call(f));

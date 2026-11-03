// A native reached through a plain function value (not a method call) takes
// bronze_dynamic_call's direct path: no NewTargetScope, no entry scope. These
// cases cover the natives that must stay off it (class constructors called
// without new, Object, natives needing an arity pad) and those that throw.
const sqrt = Math.sqrt, abs = Math.abs, max = Math.max, min = Math.min;
let s = 0;
for (let k = 0; k < 1000; k++) s += sqrt(k) + abs(k - 5) + max(k, 7, 3) + min(k);
console.log("math", Math.round(s));
console.log("noargs", max(), min(), String(sqrt()));

// Fewer arguments than the arity.
const fromCharCode = String.fromCharCode;
const parse = parseInt;
console.log("arity", parse("ff", 16), parse("12"), fromCharCode(72, 105));

// Class constructors called without new must still throw.
for (const C of [Map, Set, WeakMap, Promise]) {
    const f = C;
    try { f(); console.log("no throw", C.name); }
    catch (e) { console.log("ctor", C.name, e instanceof TypeError); }
}

// Object, Array, String, Number, Boolean called as functions.
const O = Object, A = Array, S = String, N = Number, B = Boolean;
console.log("object", typeof O(), typeof O(1), O("x") instanceof String);
console.log("array", A(3).length, A(1, 2).join(","));
console.log("conv", S(12), N("7") + 1, B(0), B("a"));

// Bound natives and natives with a this.
const bsqrt = Math.sqrt.bind(null);
const push = Array.prototype.push;
const arr = [];
for (let i = 0; i < 5; i++) push.call(arr, bsqrt(i * i));
console.log("bound", arr.join(","));
const hasOwn = Object.prototype.hasOwnProperty;
try { hasOwn("x"); console.log("no throw"); }
catch (e) { console.log("this", e instanceof TypeError); }

// A native that throws, caught many times over.
const decode = decodeURIComponent;
let thrown = 0;
for (let i = 0; i < 200; i++) {
    try { decode("%"); } catch (e) { if (e instanceof URIError) thrown++; }
}
console.log("throws", thrown);

// Natives passed as callbacks.
console.log("cb", [1, 4, 9].map(Math.sqrt).join(","), ["1", "2", "3"].map(Number).reduce((a, b) => a + b));
const apply = Function.prototype.apply;
console.log("apply", apply.call(Math.max, null, [3, 9, 2]));

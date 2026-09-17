// ECMA-262 10.2.9 / 10.2.10 for the NATIVE builtins: every constructor, static
// and prototype method the runtime interns carries the `name` and `length` its
// clause gives it, as own, non-writable, non-enumerable, configurable data
// properties — the same two the compiler gives a function it built
// (function_name_length.js pins those).
//
// The `length` here is the clause's number, not the count a short call is
// padded to: `Object.assign` and `Math.max` are variadic and have length 2,
// `Array` and `Promise` have length 1, `Date` and `Date.UTC` have 7. An
// accessor's getter is named with the "get " prefix (10.2.9 step 5) and a
// symbol-keyed method with "[description]" (step 4). The `x.constructor.name
// === "TypeError"` idiom at the end is what real programs rely on.

const ctors = [
    Object, Function, Array, String, Number, Boolean, Symbol, BigInt,
    Error, TypeError, RangeError, SyntaxError, ReferenceError, URIError, AggregateError,
    Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry,
    Promise, Date, RegExp, Proxy, Iterator,
    ArrayBuffer, SharedArrayBuffer, DataView,
    Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
    Int32Array, Uint32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array,
];
for (const c of ctors) console.log(c.name, c.length);

function row(label, fn) {
    console.log(label, JSON.stringify(fn.name), fn.length);
}

// Array.prototype (23.1.3)
for (const k of ["at", "concat", "copyWithin", "entries", "every", "fill", "filter", "find",
                 "findIndex", "findLast", "findLastIndex", "flat", "flatMap", "forEach",
                 "includes", "indexOf", "join", "keys", "lastIndexOf", "map", "pop", "push",
                 "reduce", "reduceRight", "reverse", "shift", "slice", "some", "sort", "splice",
                 "toReversed", "toSorted", "toSpliced", "toString", "unshift", "values", "with"]) {
    row("Array.prototype." + k, Array.prototype[k]);
}
row("Array.prototype[Symbol.iterator]", Array.prototype[Symbol.iterator]);
console.log(Array.prototype[Symbol.iterator] === Array.prototype.values);
for (const k of ["from", "fromAsync", "isArray", "of"]) row("Array." + k, Array[k]);

// String.prototype (22.1.3) and String statics (22.1.2)
for (const k of ["at", "charAt", "charCodeAt", "codePointAt", "concat", "endsWith", "includes",
                 "indexOf", "isWellFormed", "lastIndexOf", "localeCompare", "normalize", "padEnd",
                 "padStart", "repeat", "slice", "split", "startsWith", "substr", "substring",
                 "toLocaleLowerCase", "toLocaleUpperCase", "toLowerCase", "toString",
                 "toWellFormed", "toUpperCase", "trim", "trimEnd", "trimStart", "valueOf",
                 "match", "matchAll", "replace", "replaceAll", "search"]) {
    row("String.prototype." + k, String.prototype[k]);
}
row("String.prototype[Symbol.iterator]", String.prototype[Symbol.iterator]);
console.log(String.prototype.toString === String.prototype.valueOf);
for (const k of ["fromCharCode", "fromCodePoint", "raw"]) row("String." + k, String[k]);

// Object statics (20.1.2) and Object.prototype (20.1.3)
for (const k of ["assign", "create", "defineProperties", "defineProperty", "entries", "freeze",
                 "fromEntries", "getOwnPropertyDescriptor", "getOwnPropertyDescriptors",
                 "getOwnPropertyNames", "getOwnPropertySymbols", "getPrototypeOf", "groupBy",
                 "hasOwn", "is", "isExtensible", "isFrozen", "isSealed", "keys",
                 "preventExtensions", "seal", "setPrototypeOf", "values"]) {
    row("Object." + k, Object[k]);
}
for (const k of ["hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "toLocaleString",
                 "toString", "valueOf"]) {
    row("Object.prototype." + k, Object.prototype[k]);
}
{
    const d = Object.getOwnPropertyDescriptor(Object.prototype, "__proto__");
    row("Object.prototype.__proto__ getter", d.get);
    row("Object.prototype.__proto__ setter", d.set);
}

// Number (21.1.2, 21.1.3) and the global functions (19.2)
for (const k of ["isFinite", "isInteger", "isNaN", "isSafeInteger", "parseFloat", "parseInt"]) {
    row("Number." + k, Number[k]);
}
for (const k of ["toExponential", "toFixed", "toPrecision", "toString", "valueOf"]) {
    row("Number.prototype." + k, Number.prototype[k]);
}
for (const f of [parseInt, parseFloat, isNaN, isFinite, encodeURI, encodeURIComponent,
                 decodeURI, decodeURIComponent, escape, unescape]) {
    row("global", f);
}
console.log(Number.parseInt === parseInt, Number.parseFloat === parseFloat);
for (const k of ["toString", "valueOf"]) row("Boolean.prototype." + k, Boolean.prototype[k]);

// Math (21.3.2)
for (const k of ["abs", "acos", "acosh", "asin", "asinh", "atan", "atan2", "atanh", "cbrt",
                 "ceil", "clz32", "cos", "cosh", "exp", "expm1", "f16round", "floor", "fround",
                 "hypot", "imul", "log", "log10", "log1p", "log2", "max", "min", "pow",
                 "random", "round", "sign", "sin", "sinh", "sqrt", "tan", "tanh", "trunc"]) {
    row("Math." + k, Math[k]);
}

// JSON (25.5), Reflect (28.1), Atomics (25.4)
row("JSON.parse", JSON.parse);
row("JSON.stringify", JSON.stringify);
for (const k of ["apply", "construct", "defineProperty", "get", "getOwnPropertyDescriptor",
                 "getPrototypeOf", "has", "ownKeys", "set", "setPrototypeOf"]) {
    row("Reflect." + k, Reflect[k]);
}
for (const k of ["add", "and", "compareExchange", "exchange", "isLockFree", "load", "or",
                 "store", "sub", "xor"]) {
    row("Atomics." + k, Atomics[k]);
}

// Promise (27.2.4, 27.2.5)
for (const k of ["all", "allSettled", "any", "race", "reject", "resolve", "try",
                 "withResolvers"]) {
    row("Promise." + k, Promise[k]);
}
for (const k of ["catch", "finally", "then"]) row("Promise.prototype." + k, Promise.prototype[k]);

// Map / Set / WeakMap / WeakSet / WeakRef / FinalizationRegistry, read off an
// instance because they are answered beside the value.
{
    const m = new Map();
    for (const k of ["clear", "delete", "entries", "forEach", "get", "has", "keys", "set",
                     "values"]) {
        row("Map.prototype." + k, m[k]);
    }
    row("Map.prototype[Symbol.iterator]", m[Symbol.iterator]);
    console.log(m[Symbol.iterator] === m.entries);
    row("Map.groupBy", Map.groupBy);
    const s = new Set();
    for (const k of ["add", "clear", "delete", "difference", "entries", "forEach", "has",
                     "intersection", "isDisjointFrom", "isSubsetOf", "isSupersetOf", "keys",
                     "symmetricDifference", "union", "values"]) {
        row("Set.prototype." + k, s[k]);
    }
    row("Set.prototype[Symbol.iterator]", s[Symbol.iterator]);
    console.log(s[Symbol.iterator] === s.values, s.keys === s.values);
    const wm = new WeakMap();
    for (const k of ["delete", "get", "has", "set"]) row("WeakMap.prototype." + k, wm[k]);
    const ws = new WeakSet();
    for (const k of ["add", "delete", "has"]) row("WeakSet.prototype." + k, ws[k]);
    const wr = new WeakRef({});
    row("WeakRef.prototype.deref", wr.deref);
    const fr = new FinalizationRegistry(() => {});
    for (const k of ["register", "unregister"]) row("FinalizationRegistry.prototype." + k, fr[k]);
}

// Typed arrays (23.2.3), ArrayBuffer (25.1), DataView (25.3)
{
    const ta = new Uint8Array(1);
    for (const k of ["at", "copyWithin", "entries", "every", "fill", "filter", "find",
                     "findIndex", "findLast", "findLastIndex", "forEach", "includes", "indexOf",
                     "join", "keys", "lastIndexOf", "map", "reduce", "reduceRight", "reverse",
                     "set", "slice", "some", "sort", "subarray", "toReversed", "toSorted",
                     "toString", "values", "with"]) {
        row("%TypedArray%.prototype." + k, ta[k]);
    }
    row("%TypedArray%.prototype[Symbol.iterator]", ta[Symbol.iterator]);
    console.log(ta[Symbol.iterator] === ta.values, ta.toString === Array.prototype.toString);
    row("Uint8Array.from", Uint8Array.from);
    row("Uint8Array.of", Uint8Array.of);
    console.log(Uint8Array.from === Float64Array.from, Uint8Array.BYTES_PER_ELEMENT);
    row("ArrayBuffer.isView", ArrayBuffer.isView);
    const ab = new ArrayBuffer(8);
    for (const k of ["resize", "slice", "transfer", "transferToFixedLength"]) {
        row("ArrayBuffer.prototype." + k, ab[k]);
    }
    const dv = new DataView(ab);
    for (const k of ["getInt8", "getUint8", "getInt16", "getUint16", "getInt32", "getUint32",
                     "getFloat32", "getFloat64", "getBigInt64", "getBigUint64", "getFloat16",
                     "setInt8", "setUint8", "setInt16", "setUint16", "setInt32", "setUint32",
                     "setFloat32", "setFloat64", "setBigInt64", "setBigUint64", "setFloat16"]) {
        row("DataView.prototype." + k, dv[k]);
    }
    const sab = new SharedArrayBuffer(8);
    for (const k of ["grow", "slice"]) row("SharedArrayBuffer.prototype." + k, sab[k]);
}

// Function.prototype (20.2.3), Symbol (20.4), BigInt (21.2), Date (21.4),
// RegExp (22.2), Error.prototype (20.5.3), the iterator prototypes (27.1)
for (const k of ["apply", "bind", "call", "toString"]) {
    row("Function.prototype." + k, Function.prototype[k]);
}
row("Function.prototype[Symbol.hasInstance]", Function.prototype[Symbol.hasInstance]);
row("Function.prototype", Function.prototype);
for (const k of ["for", "keyFor"]) row("Symbol." + k, Symbol[k]);
for (const k of ["toString", "valueOf"]) row("Symbol.prototype." + k, Symbol.prototype[k]);
row("Symbol.prototype.description getter",
    Object.getOwnPropertyDescriptor(Symbol.prototype, "description").get);
for (const k of ["asIntN", "asUintN"]) row("BigInt." + k, BigInt[k]);
for (const k of ["toString", "toLocaleString", "valueOf"]) {
    row("BigInt.prototype." + k, BigInt.prototype[k]);
}
for (const k of ["now", "parse", "UTC"]) row("Date." + k, Date[k]);
for (const k of ["getDate", "getDay", "getFullYear", "getHours", "getMilliseconds",
                 "getMinutes", "getMonth", "getSeconds", "getTime", "getTimezoneOffset",
                 "getUTCDate", "getUTCDay", "getUTCFullYear", "getUTCHours",
                 "getUTCMilliseconds", "getUTCMinutes", "getUTCMonth", "getUTCSeconds",
                 "getYear", "setDate", "setFullYear", "setHours", "setMilliseconds",
                 "setMinutes", "setMonth", "setSeconds", "setTime", "setUTCDate",
                 "setUTCFullYear", "setUTCHours", "setUTCMilliseconds", "setUTCMinutes",
                 "setUTCMonth", "setUTCSeconds", "setYear", "toDateString", "toGMTString",
                 "toISOString", "toJSON", "toLocaleDateString", "toLocaleString",
                 "toLocaleTimeString", "toString", "toTimeString", "toUTCString",
                 "valueOf"]) {
    row("Date.prototype." + k, Date.prototype[k]);
}
row("Date.prototype[Symbol.toPrimitive]", Date.prototype[Symbol.toPrimitive]);
{
    const re = /x/;
    for (const k of ["exec", "test", "toString"]) row("RegExp.prototype." + k, re[k]);
    for (const s of [Symbol.match, Symbol.matchAll, Symbol.replace, Symbol.search,
                     Symbol.split]) {
        row("RegExp.prototype[" + s.description + "]", re[s]);
    }
    row("RegExp.escape", RegExp.escape);
}
row("Error.prototype.toString", Error.prototype.toString);
{
    const it = [].values();
    const proto = Object.getPrototypeOf(Object.getPrototypeOf(it));
    for (const k of ["drop", "every", "filter", "find", "flatMap", "forEach", "map", "reduce",
                     "some", "take", "toArray"]) {
        row("Iterator.prototype." + k, proto[k]);
    }
    row("Iterator.prototype[Symbol.iterator]", proto[Symbol.iterator]);
    row("Iterator.from", Iterator.from);
    row("%ArrayIteratorPrototype%.next", it.next);
    row("%MapIteratorPrototype%.next", new Map().keys().next);
    row("%StringIteratorPrototype%.next", ""[Symbol.iterator]().next);
    // Read through an instance: the members are inherited from
    // %GeneratorPrototype% / %AsyncGeneratorPrototype% either way.
    function* g() {}
    const gi = g();
    for (const k of ["next", "return", "throw"]) row("%GeneratorPrototype%." + k, gi[k]);
    async function* ag() {}
    const agi = ag();
    for (const k of ["next", "return", "throw"]) row("%AsyncGeneratorPrototype%." + k, agi[k]);
    row("%AsyncIteratorPrototype%[Symbol.asyncIterator]", agi[Symbol.asyncIterator]);
    console.log(Object.getPrototypeOf(g).constructor.name,
                Object.getPrototypeOf(g).constructor.length,
                Object.getPrototypeOf(ag).constructor.name,
                Object.getPrototypeOf(async function () {}).constructor.name);
}

// The attributes, and the own-key listing.
for (const [label, fn] of [["Array", Array], ["Array.prototype.map", Array.prototype.map],
                           ["Math.max", Math.max], ["parseInt", parseInt],
                           ["TypeError", TypeError], ["Map", Map]]) {
    const n = Object.getOwnPropertyDescriptor(fn, "name");
    const l = Object.getOwnPropertyDescriptor(fn, "length");
    console.log(label, "name", JSON.stringify(n.value), n.writable, n.enumerable, n.configurable,
                "length", l.value, l.writable, l.enumerable, l.configurable);
}
console.log(Object.getOwnPropertyNames(Array).sort().join(","));
// `prototype` is filtered out of the two method listings: a native METHOD is
// not a constructor and has no `prototype` (10.3 step 5 only gives one to a
// constructor), and whether bronze's natives carry a constructor bit is a
// separate question from what they are named.
const ownNoProto = (f) => Object.getOwnPropertyNames(f).filter((k) => k !== "prototype");
console.log(ownNoProto(Array.prototype.map).sort().join(","));
console.log(ownNoProto(parseInt).sort().join(","));
console.log("name" in Math.max, Object.hasOwn(Math.max, "length"), Object.keys(Math.max).length);
{
    // Non-writable: a sloppy assignment is discarded, a strict one throws.
    const f = Array.prototype.push;
    f.name = "renamed";
    f.length = 99;
    console.log(f.name, f.length);
    let threw = false;
    try {
        (function () { "use strict"; f.name = "x"; })();
    } catch (e) {
        threw = e instanceof TypeError;
    }
    console.log(threw);
}

// The idiom. (A Map's and a RegExp's `constructor` are not here: bronze has no
// prototype OBJECT for either yet, so the read is a named refusal.)
console.log(new TypeError("x").constructor.name, [].constructor.name,
            ({}).constructor.name, (() => {}).constructor.name,
            new RangeError("r").constructor.name, "s".constructor.name,
            (1).constructor.name, true.constructor.name, Symbol().constructor.name,
            new Date(0).constructor.name,
            Promise.resolve().constructor.name, new Uint8Array(0).constructor.name,
            new ArrayBuffer(0).constructor.name, new DataView(new ArrayBuffer(0)).constructor.name,
            1n.constructor.name, new Error("e").constructor.name);
class MyErr extends TypeError {}
console.log(new MyErr("m").constructor.name, MyErr.name, MyErr.length);
console.log(String(Array.prototype.push), String(Math.max), String(Function.prototype));

// `typeof x === "object"` and `typeof x === "function"` (and `!==`) are
// answered inline from the operand's tag and heap kind: "function" for a
// Function kind, "object" for null and every other object kind, with
// bronze_typeof asked only for a proxy, whose answer its target decides.
// Every kind of heap object and every primitive passes through the same sites,
// so a mapping wrong for any one of them shows here.
class K { constructor() { this.a = 1; } m() { return 1; } static s() { return 2; } }
class Sub extends K {}
function plainFn() {}
const arrow = () => 1;
async function asyncFn() {}
function* genFn() {}
async function* asyncGenFn() {}
const bound = plainFn.bind(null);
const boundClass = K.bind(null);
const proxyFn = new Proxy(plainFn, {});
const proxyObj = new Proxy({}, {});
const proxyArr = new Proxy([], {});
const proxyProxyFn = new Proxy(proxyFn, {});
const revocable = Proxy.revocable(function () {}, {});
const revocableObj = Proxy.revocable({}, {});
revocable.revoke();
revocableObj.revoke();
const ns = (function () { return arguments; })(1, 2);

const values = [
    ["undefined", undefined], ["null", null], ["true", true], ["false", false],
    ["int", 7], ["double", 1.5], ["nan", NaN], ["negzero", -0], ["inf", -Infinity],
    ["string", "s"], ["empty", ""], ["symbol", Symbol("q")], ["bigint", 10n],
    ["plain", { a: 1 }], ["nullproto", Object.create(null)], ["array", [1, 2]], ["holey", [, 1]],
    ["class", K], ["subclass", Sub], ["instance", new K()], ["method", K.prototype.m], ["static", K.s],
    ["fn", plainFn], ["arrow", arrow], ["async", asyncFn], ["gen", genFn], ["asyncgen", asyncGenFn],
    ["genobj", genFn()], ["promise", asyncFn()], ["bound", bound], ["boundclass", boundClass],
    ["builtin", Math.max], ["ctor", Array], ["Function", Function], ["Object", Object],
    ["proxyfn", proxyFn], ["proxyobj", proxyObj], ["proxyarr", proxyArr], ["proxyproxyfn", proxyProxyFn],
    ["revokedfn", revocable.proxy], ["revokedobj", revocableObj.proxy],
    ["date", new Date(0)], ["regexp", /x/g], ["map", new Map()], ["set", new Set()],
    ["weakmap", new WeakMap()], ["weakref", new WeakRef({})], ["error", new Error("e")],
    ["typed", new Uint8Array(4)], ["f64arr", new Float64Array(2)], ["buffer", new ArrayBuffer(8)],
    ["dataview", new DataView(new ArrayBuffer(8))], ["arguments", ns], ["iter", [1][Symbol.iterator]()],
    ["mapiter", new Map().entries()], ["boxednum", Object(1)], ["boxedstr", Object("s")],
    ["boxedsym", Object(Symbol("b"))], ["boxedbig", Object(5n)], ["json", JSON], ["math", Math],
    ["globalThis", globalThis],
];

function isObj(x) { return typeof x === "object"; }
function isFn(x) { return typeof x === "function"; }
function notObj(x) { return typeof x !== "object"; }
function notFn(x) { return "function" !== typeof x; }
function branchy(x) {
    if (typeof x === "function") return "F";
    if (typeof x === "object") return x === null ? "N" : "O";
    return "P";
}
// One typeof, compared against both kind names and a primitive one.
function both(x) {
    const t = typeof x;
    return (t === "object" ? 1 : 0) + (t === "function" ? 2 : 0) + (t === "number" ? 4 : 0);
}
// A typeof read by something other than a compare keeps the string.
function named(x) { const t = typeof x; return t === "object" ? "obj:" + t : t; }

for (let rep = 0; rep < 3; rep++) {
    for (const [name, v] of values) {
        const line = [name, isObj(v), isFn(v), notObj(v), notFn(v), branchy(v), both(v), named(v)].join(" ");
        if (rep === 2) console.log(line);
    }
}

// A hot loop over mixed kinds, under GC pressure between passes.
let h = 0;
for (let r = 0; r < 200; r++) {
    for (let i = 0; i < values.length; i++) {
        const v = values[i][1];
        h = (h * 31 + (isObj(v) ? 1 : 0) + (isFn(v) ? 2 : 0) + both(v)) | 0;
    }
    if (r % 50 === 0) { const junk = []; for (let j = 0; j < 2000; j++) junk.push({ j }); }
}
console.log("hash", h);

// `typeof x === "lit"` and its siblings lower to typeof.is (a tag test, and
// for "object"/"function" a callable check, a proxy's asked of the runtime).
// Every value kind against every literal, every operator, either order.
const names = ["undefined", "object", "boolean", "number", "string", "symbol", "bigint", "function", "Object", "", "nonsense"];
class K {}
function* gen() {}
async function af() {}
const values = [
    undefined, null, true, false, 0, -0, 1.5, NaN, Infinity, 2147483647, -2147483648, 4294967296,
    "", "a", "é", "😀", "\ud800", Symbol("s"), 10n, -(2n ** 70n),
    {}, [], [1, , 3], function () {}, () => 1, K, gen, af, Math.max, new Date(0), /x/,
    new Map(), new Proxy({}, {}), new Proxy(function () {}, {}), new Proxy(() => 1, {}),
    Proxy.revocable(function () {}, {}), new Uint8Array(2), Object.create(null), new String("s"), new Number(1),
    Object(1n), Object(Symbol("w")), new Boolean(false), K.prototype, Function.prototype, globalThis,
];
const r = Proxy.revocable(function () {}, {});
r.revoke();
values.push(r.proxy);
const r2 = Proxy.revocable({}, {});
r2.revoke();
values.push(r2.proxy);

function classify(v) {
    let s = "";
    if (typeof v === "undefined") s += "u";
    if (typeof v === "object") s += "o";
    if (typeof v === "boolean") s += "b";
    if (typeof v === "number") s += "n";
    if (typeof v === "string") s += "s";
    if (typeof v === "symbol") s += "y";
    if (typeof v === "bigint") s += "g";
    if (typeof v === "function") s += "f";
    if (typeof v === "Object") s += "X";
    if ("function" == typeof v) s += "F";
    if ("object" != typeof v) s += "!o";
    if (typeof v !== "number") s += "!n";
    if (typeof v == "string") s += "S";
    return s;
}
let out = [];
for (let i = 0; i < values.length; i++) out.push(i + ":" + classify(values[i]) + ":" + typeof values[i]);
console.log(out.join(" "));

// A hole read and a missing property are undefined.
const holey = [1, , 3];
console.log(typeof holey[1] === "undefined", typeof ({}).nope === "undefined", typeof undeclaredGlobal === "undefined");

// Hot loop over mixed values, so a harvested build sees every arm.
let counts = {};
for (let round = 0; round < 2000; round++) {
    for (const v of values) {
        for (const n of names) {
            if (typeof v === n) counts[n] = (counts[n] || 0) + 1;
        }
        if (typeof v === "function") counts.F = (counts.F || 0) + 1;
        if (typeof v !== "object") counts.notObj = (counts.notObj || 0) + 1;
        if (typeof v == "number") counts.N = (counts.N || 0) + 1;
    }
}
console.log(JSON.stringify(counts));

// The comparison's result as a value, and its operands' side effects.
let log = [];
const f = (x) => { log.push("f"); return x; };
const t = (s) => { log.push("t"); return s; };
console.log(typeof f(1) === t("number"), t("string") === typeof f("x"), log.join(""));
// A non-literal operand keeps the general compare.
const want = "num" + "ber";
console.log(typeof 5 === want, typeof "5" === want);

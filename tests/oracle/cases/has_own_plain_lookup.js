// hasOwnProperty / propertyIsEnumerable on a plain receiver read the shape
// without interning the key; `in` on a plain receiver likewise. Every other
// receiver kind and key kind must give the spec's answer.

const sym = Symbol("s");
const o = { a: 1, "1": 2, [sym]: 3, get g() { return 4; } };
Object.defineProperty(o, "hidden", { value: 5, enumerable: false });
const keys = ["a", "1", 1, "g", "hidden", "missing", "toString", sym, Symbol("s"), "__proto__"];
const fmt = (k) => typeof k === "symbol" ? k.toString() : JSON.stringify(k);
for (const k of keys) {
  console.log("plain", fmt(k), o.hasOwnProperty(k), o.propertyIsEnumerable(k), k in o);
}

// Heap strings (built at run time) as keys.
const built = ["h", "id", "den"].join("").slice(1);
console.log("built", built, o.hasOwnProperty("h" + built), "hid" + "den" in o);
const ks = []; for (let i = 0; i < 5; i++) ks.push("k" + i);
const dyn = {}; for (const k of ks) dyn[k] = k;
console.log(ks.map((k) => dyn.hasOwnProperty(k) && k in dyn).join(","), dyn.hasOwnProperty("k" + 9));

// String wrappers: indices and length are own.
const sw = new String("abc");
for (const k of ["0", "2", "3", "length", 1, "x"]) {
  console.log("wrapper", fmt(k), sw.hasOwnProperty(k), sw.propertyIsEnumerable(k), k in sw);
}

// Arrays, functions, null-prototype, frozen, dictionary (after delete).
const arr = [1, , 3];
console.log("array", arr.hasOwnProperty(1), arr.hasOwnProperty(2), arr.hasOwnProperty("length"),
  arr.propertyIsEnumerable("length"), 1 in arr, "length" in arr);
function fn(x) {}
console.log("function", fn.hasOwnProperty("name"), fn.propertyIsEnumerable("prototype"), "call" in fn);
const nul = Object.create(null); nul.x = 1;
console.log("null proto", Object.prototype.hasOwnProperty.call(nul, "x"), "x" in nul, "toString" in nul);
const fr = Object.freeze({ f: 1 });
console.log("frozen", fr.hasOwnProperty("f"), fr.propertyIsEnumerable("f"), "f" in fr);
const dict = { a: 1, b: 2, c: 3 }; delete dict.b;
for (let i = 0; i < 40; i++) dict["p" + i] = i;
console.log("dict", dict.hasOwnProperty("b"), dict.hasOwnProperty("c"), dict.hasOwnProperty("p39"),
  "b" in dict, "p7" in dict, dict.propertyIsEnumerable("p1"));
Object.defineProperty(dict, "p1", { enumerable: false });
console.log("dict nonenum", dict.propertyIsEnumerable("p1"), dict.hasOwnProperty("p1"));

// Inherited, shadowing, accessor on the prototype.
class Base { get acc() { return 1; } }
class Sub extends Base { constructor() { super(); this.own = 1; } }
const s = new Sub();
console.log("class", s.hasOwnProperty("own"), s.hasOwnProperty("acc"), "acc" in s, s.propertyIsEnumerable("own"));

// Proxies: traps fire.
const trapLog = [];
const px = new Proxy({ t: 1 }, {
  has(t, k) { trapLog.push("has " + String(k)); return Reflect.has(t, k); },
  getOwnPropertyDescriptor(t, k) { trapLog.push("gopd " + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
});
console.log("proxy", px.hasOwnProperty("t"), "t" in px, "u" in px, Object.prototype.propertyIsEnumerable.call(px, "t"));
const behind = Object.create(px); behind.own = 1;
console.log("proxy chain", "t" in behind, "own" in behind, "zz" in behind);
console.log(trapLog.join("|"));

// Same shape, key toggled by delete and redefinition (memo must not go stale).
function mk() { return { m: 1, n: 2 }; }
for (let round = 0; round < 3; round++) {
  const x = mk();
  if (round === 1) delete x.m;
  if (round === 2) Object.defineProperty(x, "m", { enumerable: false });
  console.log("round", round, x.hasOwnProperty("m"), x.propertyIsEnumerable("m"), "m" in x, "n" in x);
}

// Non-object receivers via call, and ToPropertyKey on objects.
console.log(Object.prototype.hasOwnProperty.call("str", "length"), Object.prototype.hasOwnProperty.call(5, "x"));
const keyObj = { toString() { return "a"; } };
console.log(o.hasOwnProperty(keyObj), keyObj in o);
try { Object.prototype.hasOwnProperty.call(null, "x"); } catch (e) { console.log(e.constructor.name); }
try { "x" in 5; } catch (e) { console.log(e.constructor.name); }

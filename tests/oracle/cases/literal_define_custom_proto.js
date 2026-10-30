// `__proto__: v` in an object literal sets the new object's prototype
// (B.3.1, 13.2.5.5 step 6) — and only that spelling: a computed
// `["__proto__"]`, a shorthand `{ __proto__ }` and a method `__proto__() {}`
// each define an ordinary own property named "__proto__". Whatever the
// prototype holds, the literal's other properties are still DEFINED, before
// or after the `__proto__` entry: a setter on the new prototype is never run.

const log = [];
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

const P = {
  set x(v) {
    log.push("P set x " + v);
  },
  get x() {
    return "P x";
  },
  tag: "P",
};
Object.defineProperty(P, "ro", { value: "P ro", writable: false });

// The prototype entry first, then properties the prototype intercepts.
const a = { __proto__: P, x: 1, ro: 2 };
console.log("proto first", Object.getPrototypeOf(a) === P, a.tag, flush(), own(a, "x"), own(a, "ro"));
// The same with the prototype entry last.
const b = { x: 3, ro: 4, __proto__: P };
console.log("proto last", Object.getPrototypeOf(b) === P, flush(), own(b, "x"), own(b, "ro"));
// Assignment through the prototype afterwards still meets the setter on a
// fresh object, which is the contrast.
const c = { __proto__: P };
c.x = 5;
c.ro = 6;
console.log("assigned", flush(), own(c, "x"), own(c, "ro"), c.ro);

// The quoted spelling is the colon form too.
const quoted = { "__proto__": P, x: 7 };
console.log("quoted", flush(), Object.getPrototypeOf(quoted) === P, Object.keys(quoted).join(","));

// A value that is neither an object nor null is ignored: the prototype stays
// Object.prototype and no property is made.
const ignored = { __proto__: 42, y: 1 };
console.log("ignored", Object.getPrototypeOf(ignored) === Object.prototype, Object.keys(ignored).join(","));
const nullProto = { __proto__: null, y: 2 };
console.log("null", Object.getPrototypeOf(nullProto) === null, nullProto.y);

// The three spellings that DEFINE an own "__proto__" instead.
const __proto__ = P;
const shorthand = { __proto__ };
console.log("shorthand", Object.getPrototypeOf(shorthand) === Object.prototype,
  Object.keys(shorthand).join(","), Object.getOwnPropertyDescriptor(shorthand, "__proto__").value === P);
const computed = { ["__proto__"]: P };
console.log("computed", Object.getPrototypeOf(computed) === Object.prototype,
  Object.keys(computed).join(","), Object.getOwnPropertyDescriptor(computed, "__proto__").value === P);
const method = { __proto__() { return "method"; } };
console.log("method", Object.getPrototypeOf(method) === Object.prototype,
  Object.keys(method).join(","), typeof Object.getOwnPropertyDescriptor(method, "__proto__").value);

// A prototype chain two links deep, with the setter on the far link.
const Q = { __proto__: P, tag: "Q" };
const deep = { __proto__: Q, x: 8, ro: 9 };
console.log("deep", deep.tag, flush(), own(deep, "x"), own(deep, "ro"));

// A spread onto a literal whose prototype intercepts the copied keys.
const spread = { __proto__: P, ...{ x: 10, ro: 11 } };
console.log("spread", flush(), own(spread, "x"), own(spread, "ro"));

// A proxy on the chain is not asked either: no trap runs for a definition.
const trapped = new Proxy({}, {
  set(t, k, v) {
    log.push("trap set " + String(k));
    return true;
  },
  defineProperty(t, k, d) {
    log.push("trap define " + String(k));
    return true;
  },
});
const viaProxy = { __proto__: trapped, x: 12, ro: 13 };
console.log("proxy", flush(), own(viaProxy, "x"), own(viaProxy, "ro"));
viaProxy.z = 14;
console.log("proxy assignment", flush(), own(viaProxy, "z"));

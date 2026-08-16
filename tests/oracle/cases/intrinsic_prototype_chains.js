// Every intrinsic prototype ends at `Object.prototype`, so every intrinsic
// INSTANCE answers the four members 20.1.3 gives every object.
//
// That is one sentence about the language and it was two different arrangements
// here. Where a receiver's members come from a table beside the value — an
// array, a Map, a RegExp — the property path takes a chain-end step at the end
// of the table, and those instances answered `hasOwnProperty` all along. Where
// the receiver is an ORDINARY object whose prototype is a real object — a
// promise, an error — there is no table and no chain-end step: the walk simply
// ended, and `Promise.resolve(1).hasOwnProperty` was `undefined`. This case
// pins both kinds against the same question so the two cannot drift apart
// again.
//
// The error half brings 20.5.3.4 `Error.prototype.toString` with it, and it has
// to: once `Error.prototype`'s parent is `Object.prototype`, an error with no
// `toString` of its own would find `Object.prototype`'s and `String(err)` would
// read "[object Object]" — a wrong answer arriving as a side effect of fixing
// the chain, where before it was a "cannot convert to primitive" refusal.
// 20.5.3.4 reads `name` and `message` through the ordinary walk, so a subclass
// that sets neither is answered from `Error.prototype`'s pair, which the
// `MyErr` line pins.
//
// The brand checks are pinned beside them because the chain is exactly what
// could have broken them: a method that is on a chain is a method that can be
// `.call`ed on the wrong receiver, and the answer must still be a TypeError.
console.log(Object.getPrototypeOf(Promise.prototype) === Object.prototype);
console.log(Object.getPrototypeOf(Error.prototype) === Object.prototype);
console.log(Object.getPrototypeOf(TypeError.prototype) === Error.prototype);
console.log(Object.getPrototypeOf(RangeError.prototype) === Error.prototype);

const p = Promise.resolve(1);
console.log(typeof p.hasOwnProperty, typeof p.isPrototypeOf, typeof p.propertyIsEnumerable);
console.log(p.hasOwnProperty("then"), Promise.prototype.hasOwnProperty("then"));
console.log(Object.getPrototypeOf(p) === Promise.prototype, Promise.prototype.isPrototypeOf(p));
console.log(Object.keys(p).length, JSON.stringify(p));
console.log(Object.prototype.toString.call(p));

const err = new TypeError("boom");
console.log(typeof err.hasOwnProperty, typeof err.toString, typeof err.isPrototypeOf);
console.log(String(err));
console.log(String(new Error()));
console.log(String(new RangeError("r")));
console.log(err.hasOwnProperty("message"), err.hasOwnProperty("name"));
console.log(err.propertyIsEnumerable("message"), Object.keys(err).length, JSON.stringify(err));
console.log(Object.prototype.toString.call(err));
console.log(Error.prototype.isPrototypeOf(err), err instanceof TypeError, err instanceof Error);

class MyErr extends Error {}
const sub = new MyErr("m");
console.log(sub instanceof Error, String(sub));

// The kinds whose members are answered beside the value: unchanged, and pinned
// so the chain-end step cannot quietly go away.
const beside = [
  new Map(),
  new Set(),
  new WeakMap(),
  new WeakSet(),
  /a/,
  new ArrayBuffer(4),
  new DataView(new ArrayBuffer(4)),
  new Float64Array(1),
  new Uint8Array(1),
  Symbol("s"),
  new Date(0),
  [],
  function () {},
];
console.log(
  beside
    .map(function (v) {
      return typeof v.hasOwnProperty;
    })
    .join(",")
);

function* gen() {}
async function* agen() {}
console.log(
  [gen(), agen(), [][Symbol.iterator](), ""[Symbol.iterator](), new Map()[Symbol.iterator]()]
    .map(function (v) {
      return typeof v.hasOwnProperty;
    })
    .join(",")
);

// Brand checks, which a chain does not weaken.
const mapGet = new Map().get;
try {
  mapGet.call({});
} catch (e) {
  console.log("Map.get brand:", e instanceof TypeError);
}
console.log(Object.keys(new Map()).length, JSON.stringify(new Map()));
try {
  Promise.prototype.then.call({});
} catch (e) {
  console.log("Promise.then brand:", e instanceof TypeError);
}

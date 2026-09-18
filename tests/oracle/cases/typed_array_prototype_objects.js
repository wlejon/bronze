// `%TypedArray%`, `%TypedArray%.prototype` and the twelve view constructors
// as OBJECTS (ECMA-262 23.2).
//
// Each used to be a C table standing in for the prototype: a view had no
// [[Prototype]] a program could read, `Uint8Array.prototype` was a named
// refusal, and `class extends Float32Array` kept its prototype on a side box.
// Now `%TypedArray%` is an abstract constructor (23.2.1: a TypeError whether
// called or constructed), `%TypedArray%.prototype` is an ordinary object
// holding every shared method and the four accessors (23.2.3), each view
// constructor has `%TypedArray%` as its [[Prototype]] (23.2.6) and a
// prototype of its own holding only `constructor` and `BYTES_PER_ELEMENT`
// (23.2.7), and every instance is an ordinary object with internal slots
// whose [[Prototype]] comes from NewTarget. So the case pins what a program
// can now see and could not before: the chain's identities, the sorted
// own-name lists, the descriptors and name/length of the members, the
// species-driven `map`/`filter`/`slice`/`subarray`, a wrong receiver being a
// TypeError rather than a crash, and the ordinary object machinery —
// expandos, `Object.keys`, JSON, `for-in`, `setPrototypeOf` — treating a
// view as the object it is while its indices keep 10.4.5's rules.

const names = (o) => Object.getOwnPropertyNames(o).sort().join(",");
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
const TA = Object.getPrototypeOf(Uint8Array);
const TAP = TA.prototype;

// --- the family: one abstract constructor, one shared prototype ------------
console.log(typeof TA, TA.name, TA.length, TA === Object.getPrototypeOf(Int32Array), TA === Object.getPrototypeOf(Float64Array), TA === Object.getPrototypeOf(BigInt64Array));
console.log(TAP === Object.getPrototypeOf(Uint8Array.prototype), TAP === Object.getPrototypeOf(Float32Array.prototype), Object.getPrototypeOf(TAP) === Object.prototype, Object.getPrototypeOf(TA) === Function.prototype);
console.log(fails(() => TA()), fails(() => new TA()), fails(() => new TA(4)), fails(() => Reflect.construct(TA, [], Uint8Array)));
console.log(names(TAP));
console.log(names(TA), TA.prototype === TAP, TAP.constructor === TA);
console.log(desc(TA, "prototype"), desc(TAP, "constructor"), desc(TAP, "map"), desc(TAP, "length"), desc(TAP, "buffer"), desc(TAP, "byteLength"), desc(TAP, "byteOffset"));
console.log(desc(TAP, Symbol.toStringTag), desc(TAP, Symbol.iterator), desc(TA, Symbol.species), desc(TA, "from"), desc(TA, "of"));
console.log(TAP[Symbol.iterator] === TAP.values, TAP.toString === Array.prototype.toString, TAP[Symbol.toStringTag], TA[Symbol.species] === TA);
console.log(Object.getOwnPropertySymbols(TAP).map(String).join(","), Object.getOwnPropertySymbols(TA).map(String).join(","));

// --- name and length of every shared member (23.2.3) -------------------------
const members = [
  ["at", 1], ["copyWithin", 2], ["entries", 0], ["every", 1], ["fill", 1], ["filter", 1],
  ["find", 1], ["findIndex", 1], ["findLast", 1], ["findLastIndex", 1], ["forEach", 1],
  ["includes", 1], ["indexOf", 1], ["join", 1], ["keys", 0], ["lastIndexOf", 1], ["map", 1],
  ["reduce", 1], ["reduceRight", 1], ["reverse", 0], ["set", 1], ["slice", 2], ["some", 1],
  ["sort", 1], ["subarray", 2], ["toLocaleString", 0], ["toReversed", 0], ["toSorted", 1],
  ["values", 0], ["with", 2],
];
console.log(members.map(([n, l]) => n + ":" + (TAP[n].name === n) + ":" + (TAP[n].length === l)).join(" "));
console.log(TAP[Symbol.iterator].name, TA.from.length, TA.of.length, TA.from.name, TA.of.name);
console.log(Object.getOwnPropertyDescriptor(TAP, "length").get.name, Object.getOwnPropertyDescriptor(TAP, "buffer").get.name, Object.getOwnPropertyDescriptor(TAP, Symbol.toStringTag).get.name);
console.log(Object.getOwnPropertyDescriptor(TAP, "length").get.length, Object.getOwnPropertyDescriptor(TAP, "byteLength").set, Object.getOwnPropertyDescriptor(TAP, Symbol.toStringTag).get.call(new Int16Array(1)), Object.getOwnPropertyDescriptor(TAP, Symbol.toStringTag).get.call({}));

// --- the twelve constructors (23.2.6 / 23.2.7) --------------------------------
const ctors = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array];
for (const C of ctors) {
  const v = new C(2);
  console.log(C.name, C.length, C.BYTES_PER_ELEMENT, C.prototype.BYTES_PER_ELEMENT, names(C), names(C.prototype), Object.getPrototypeOf(v) === C.prototype, v instanceof C, C.prototype.constructor === C, Object.prototype.toString.call(v), v[Symbol.toStringTag], desc(C, "BYTES_PER_ELEMENT"), desc(C.prototype, "BYTES_PER_ELEMENT"), desc(C.prototype, "constructor"), C.from === TA.from, C[Symbol.species] === C);
}
console.log(typeof Float16Array === "undefined" ? "no Float16Array" : Float16Array.BYTES_PER_ELEMENT);
console.log(Uint8Array.prototype.__proto__ === Object.getPrototypeOf(Int32Array.prototype), Object.getPrototypeOf(Uint8Array) === Object.getPrototypeOf(Int32Array), Uint8Array.prototype.map === Int32Array.prototype.map, Uint8Array.prototype.hasOwnProperty("map"));
console.log(fails(() => Uint8Array()), fails(() => Int32Array(4)), fails(() => Float32Array.call({}, 4)));
console.log(Object.prototype.toString.call(TAP), Object.prototype.toString.call(Uint8Array.prototype), Uint8Array.prototype[Symbol.toStringTag], String(Uint8Array.prototype[Symbol.toStringTag]));

// --- an instance: indices are 10.4.5's, names are ordinary ------------------
const v = new Uint8Array([1, 2, 3]);
console.log(v.length, v.byteLength, v.byteOffset, v.buffer instanceof ArrayBuffer, v.buffer.byteLength, v.hasOwnProperty("length"), v.hasOwnProperty("0"), v.hasOwnProperty("3"), "1" in v, "3" in v, "-0" in v, "1.5" in v, "map" in v);
console.log(Object.getOwnPropertyNames(v).join(","), Object.keys(v).join(","), JSON.stringify(Object.getOwnPropertyDescriptor(v, "1")), Object.getOwnPropertyDescriptor(v, "5"), Object.getOwnPropertyDescriptor(v, "-1"));
v.tag = "expando";
v[7] = 99;
v["1.5"] = 5;
v[-1] = 8;
console.log(v.tag, v[7], v["1.5"], v[-1], v.length, Object.keys(v).join(","), names(v), JSON.stringify(v), JSON.stringify({ v }));
console.log(delete v[0], delete v[9], delete v["-0"], delete v.tag, v[0], v.tag, Object.keys(v).join(","));
Object.defineProperty(v, "hidden", { value: 3, enumerable: false });
console.log(names(v), v.hidden, Object.getOwnPropertyDescriptor(v, "hidden").enumerable);
console.log(fails(() => Object.defineProperty(v, "0", { get() { return 1; } })), fails(() => Object.defineProperty(v, "0", { value: 1, enumerable: false })), fails(() => Object.defineProperty(v, "9", { value: 1 })), Object.defineProperty(v, "0", { value: 42 })[0], Object.defineProperty(v, "1", { value: 7, writable: true, enumerable: true, configurable: true })[1]);
let forIn = "";
for (const k in v) forIn += k + ";";
console.log(forIn);
const spread = { ...new Int8Array([4, 5]) };
console.log(JSON.stringify(spread), Reflect.ownKeys(new Int8Array(2)).join(","), Reflect.has(v, "2"), Reflect.has(v, "3"), Reflect.get(v, "1"), Reflect.set(v, "1", 9), v[1], Reflect.set(v, "9", 1), Reflect.deleteProperty(v, "9"), Reflect.deleteProperty(v, "0"));
console.log(fails(() => Object.freeze(new Uint8Array(1))), fails(() => Object.seal(new Uint8Array(1))), Object.isFrozen(Object.freeze(new Uint8Array(0))), Object.isExtensible(Object.preventExtensions(new Uint8Array(2))), Object.isFrozen(new Uint8Array(1)), Object.isSealed(new Uint8Array(1)));
const np = Object.preventExtensions(new Uint8Array(2));
np[0] = 5;
np.late = 1;
console.log(np[0], np.late, Object.isExtensible(np));

// --- methods through `.call`, and the receiver check (23.2.3, 23.2.4.3) -----
const src = new Float32Array([3, 1, 2]);
console.log(TAP.join.call(src, "-"), Uint8Array.prototype.map.call(src, (x) => x * 2).join(","), TAP.at.call(src, -1), TAP.indexOf.call(src, 2), TAP.includes.call(src, 3), TAP.reduce.call(src, (a, b) => a + b, 0));
console.log(Object.getOwnPropertyDescriptor(TAP, "length").get.call(src), Object.getOwnPropertyDescriptor(TAP, "byteLength").get.call(src), Object.getOwnPropertyDescriptor(TAP, "byteOffset").get.call(new Uint8Array(new ArrayBuffer(8), 2)), Object.getOwnPropertyDescriptor(TAP, "buffer").get.call(src) === src.buffer);
console.log(fails(() => TAP.map.call([], (x) => x)), fails(() => TAP.map.call({ length: 1 }, (x) => x)), fails(() => TAP.join.call(new DataView(new ArrayBuffer(1)))), fails(() => TAP.set.call(new ArrayBuffer(1), [1])));
console.log(fails(() => TAP.values.call(Object.create(Uint8Array.prototype))), fails(() => TAP.slice.call(undefined)), fails(() => TAP.fill.call(null, 1)), fails(() => TAP.reverse.call(1)));
console.log(fails(() => Object.getOwnPropertyDescriptor(TAP, "length").get.call({})), fails(() => Object.getOwnPropertyDescriptor(TAP, "buffer").get.call([])), fails(() => Object.getOwnPropertyDescriptor(TAP, "byteOffset").get.call(Uint8Array.prototype)), Object.getOwnPropertyDescriptor(TAP, Symbol.toStringTag).get.call(Uint8Array.prototype));
console.log(fails(() => Uint8Array.prototype.length), fails(() => TAP.byteLength), fails(() => TAP.buffer), TAP[Symbol.toStringTag], Object.prototype.toString.call(Object.create(Uint8Array.prototype)));
console.log(fails(() => TA.from.call(Object, [1])), fails(() => TA.of.call({}, 1)), TA.from.call(Int16Array, [1, 2]).join(","), TA.of.call(Uint8Array, 5, 6) instanceof Uint8Array, Uint8Array.from({ length: 2 }, (_, i) => i * 3).join(","), Int8Array.of(1, 2).length);

// --- subclassing: NewTarget decides the prototype, species the derived views --
class V extends Float32Array {
  sum() {
    let s = 0;
    for (let i = 0; i < this.length; i++) s += this[i];
    return s;
  }
}
const w = new V(4);
w[0] = 1.5;
w[3] = 2;
console.log(Object.getPrototypeOf(w) === V.prototype, w instanceof V, w instanceof Float32Array, w.sum(), w.length, w.BYTES_PER_ELEMENT, V.BYTES_PER_ELEMENT, Object.getPrototypeOf(V) === Float32Array, Object.getPrototypeOf(V.prototype) === Float32Array.prototype);
const mapped = w.map((x) => x + 1);
console.log(mapped instanceof V, mapped.constructor === V, mapped.sum(), w.filter((x) => x > 0) instanceof V, w.slice(1) instanceof V, w.subarray(2) instanceof V, w.subarray(2).buffer === w.buffer, w.toSorted() instanceof V, w.toReversed() instanceof V, w.with(0, 9) instanceof V);
console.log(V.from([1, 2]) instanceof V, V.of(3) instanceof V, V.from([1, 2]).sum(), Object.prototype.toString.call(w), w[Symbol.toStringTag], JSON.stringify(w), [...w].join(","));
class Pair extends Int16Array {
  constructor(a, b) {
    super(2);
    this[0] = a;
    this[1] = b;
  }
  get swapped() {
    return new Pair(this[1], this[0]);
  }
}
const p = new Pair(1, 2);
console.log(p.length, p.swapped.join(","), p.swapped instanceof Pair, Object.getPrototypeOf(p.map((x) => x)) === Pair.prototype, p.map((x) => x * 2).join(","));
class Plain extends Uint8Array {
  static get [Symbol.species]() {
    return Uint8Array;
  }
}
const pl = new Plain(3);
console.log(pl.map((x) => x) instanceof Plain, pl.map((x) => x) instanceof Uint8Array, pl.subarray(1).constructor === Uint8Array, pl.slice().constructor === Uint8Array);
function Other() {}
const viaOther = Reflect.construct(Uint8Array, [2], Other);
console.log(Object.getPrototypeOf(viaOther) === Other.prototype, viaOther instanceof Uint8Array, viaOther.length, TAP.join.call(viaOther, "+"), Object.getOwnPropertyDescriptor(TAP, "length").get.call(viaOther), Object.prototype.toString.call(viaOther));
console.log(fails(() => viaOther.join(",")), fails(() => viaOther.length), TAP.at.call(viaOther, 0), viaOther[1]);

// --- iteration: the prototype's `values`, replaceable ------------------------
const it = new Uint8Array([7, 8]).values();
const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
console.log(Object.getPrototypeOf(it) === ArrayIteratorPrototype, Object.prototype.toString.call(it), JSON.stringify(it.next()), JSON.stringify([...new Uint8Array([1, 2]).entries()]), [...new Uint8Array([1, 2]).keys()].join(","));
const custom = new Uint8Array([1, 2]);
custom[Symbol.iterator] = function* () { yield "custom"; };
console.log([...custom].join(","), [...new Uint8Array([1, 2])].join(","), Array.from(custom).join(","), [...TAP.values.call(custom)].join(","));
class Rev extends Uint8Array {
  *[Symbol.iterator]() { yield* [...super.values()].reverse(); }
}
console.log([...new Rev([1, 2, 3])].join(","), Array.from(new Rev([4, 5])).join(","), new Rev([1, 2]).join(","));
const savedValues = TAP.values;
TAP[Symbol.iterator] = function* () { yield "swapped"; };
console.log([...new Uint8Array(2)].join(","), Array.from(new Uint8Array(1)).join(","));
TAP[Symbol.iterator] = savedValues;
console.log([...new Uint8Array([9, 8])].join(","), TAP[Symbol.iterator] === TAP.values);

// --- an ordinary object: setPrototypeOf, Object.create, isPrototypeOf ---------
const detached = new Uint8Array([1, 2]);
Object.setPrototypeOf(detached, null);
console.log(Object.getPrototypeOf(detached), detached.map, detached[1], detached.length, TAP.join.call(detached, ","), Object.getOwnPropertyDescriptor(TAP, "length").get.call(detached));
const adopted = Object.create(Uint8Array.prototype);
console.log(adopted instanceof Uint8Array, typeof adopted.map, fails(() => adopted.map((x) => x)), fails(() => adopted.length), adopted[0], Object.keys(adopted).length);
console.log(TAP.isPrototypeOf(new Uint8Array(1)), Uint8Array.prototype.isPrototypeOf(new Int8Array(1)), Object.prototype.isPrototypeOf(new Int8Array(1)), Object.getPrototypeOf(new Uint8Array(1)) === Uint8Array.prototype);
Uint8Array.prototype.double = function () { return this.map((x) => x * 2); };
console.log(new Uint8Array([1, 2]).double().join(","), typeof new Int8Array(1).double, "double" in new Uint8Array(1), names(Uint8Array.prototype));
delete Uint8Array.prototype.double;
console.log(typeof new Uint8Array(1).double, names(Uint8Array.prototype));

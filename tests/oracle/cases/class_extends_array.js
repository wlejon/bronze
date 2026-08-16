// `class extends Array`, and the @@species dispatch that keeps a derived array
// derived through the methods that build new arrays.
//
// 23.1.1.1 allocates through ArrayCreate(0, GetPrototypeFromConstructor(
// NewTarget, "%Array.prototype%")), so the instance is a real array exotic
// object — `length` tracks the indices, writing past the end grows it, and
// shortening `length` drops the tail — carrying the SUBCLASS prototype.
//
// The methods that produce a new array do not call `Array`: 23.1.3.21 and its
// neighbours call ArraySpeciesCreate (7.3.22), which reads
// `O.constructor[@@species]` and constructs THAT. For an ordinary subclass the
// answer is the subclass, because 23.1.2.5 defines `get [@@species]` as
// `return this` and the subclass inherits that accessor through its static
// chain — so `v.map(f)` is a `Vec`, with nothing written to say so.
//
// `static get [Symbol.species]() { return Array; }` is the documented opt-out
// and this pins that it works, which is the only reason the accessor is
// specified as an accessor at all.
//
// The last block is the fast path's pin. ArraySpeciesCreate on a PLAIN array
// must not become a property read: the guard is that a plain array carries no
// property box at all, and the constructor question is only asked of one that
// does. `doubleAll` below is one generic function reached by both kinds, so a
// guard that answered by the call site rather than by the receiver would show
// up here as the wrong answer on one of the two lines.

class Vec extends Array {
  sum() {
    let t = 0;
    for (const v of this) t += v;
    return t;
  }
}

const v = new Vec();
v.push(1, 2, 3);
console.log(v.length, v[0], v[2], v.sum());

v[5] = 9;
console.log(v.length, v[3], v[5]);

v.length = 3;
console.log(v.length, v[5], v.join(","));

console.log(Array.isArray(v), v instanceof Vec, v instanceof Array);
console.log(Object.prototype.toString.call(v), JSON.stringify(v));

// ArraySpeciesCreate, five ways.
const mapped = v.map((x) => x * 2);
console.log(mapped instanceof Vec, mapped.join(","), mapped.sum());
const filtered = v.filter((x) => x > 1);
const sliced = v.slice(1);
const cat = v.concat([4]);
console.log(filtered instanceof Vec, sliced instanceof Vec, cat instanceof Vec, cat.join(","));

const nested = new Vec();
nested.push(1, [2, 3]);
const flat = nested.flat();
console.log(flat instanceof Vec, flat.join(","), flat.length);

// The single-argument `new Array(n)` form, through the subclass.
class Sized extends Array {}
const z = new Sized(3);
console.log(z.length, z[0], z instanceof Sized, Array.isArray(z));
const z2 = new Sized(1, 2);
console.log(z2.length, z2.join(","), z2 instanceof Sized);

// The opt-out.
class Plainish extends Array {
  static get [Symbol.species]() {
    return Array;
  }
}
const p = new Plainish();
p.push(1, 2, 3);
const pm = p.map((x) => x + 1);
console.log(p instanceof Plainish, pm instanceof Plainish, pm instanceof Array, pm.join(","));

// One generic function, both receivers.
function doubleAll(xs) {
  return xs.map((x) => x * 2);
}
const plainArr = [1, 2, 3];
const subArr = new Vec();
subArr.push(1, 2, 3);
const a1 = doubleAll(plainArr);
const a2 = doubleAll(subArr);
console.log(a1.join(","), a1 instanceof Vec, Array.isArray(a1));
console.log(a2.join(","), a2 instanceof Vec, Array.isArray(a2));

// The same question again with the call site HOT, in both orders, because an
// inline cache is the thing that could get it wrong: a site that has seen two
// thousand plain arrays holds a filled array-method cache, and the subclass
// arriving after it must miss rather than be answered from it. The cache is
// filled only for a receiver with no property box, which every subclass
// instance has and no plain array does — the same guard the species dispatch
// uses, asserted here from the other side.
function dbl(xs) {
  return xs.map((x) => x * 2);
}
let hot = null;
for (let k = 0; k < 2000; k++) hot = dbl(plainArr);
console.log(hot.join(","), hot instanceof Vec);
console.log(dbl(subArr).join(","), dbl(subArr) instanceof Vec);
console.log(dbl(plainArr).join(","), dbl(plainArr) instanceof Vec);

function keep(xs) {
  return xs.filter((x) => x > 1);
}
for (let k = 0; k < 2000; k++) keep(subArr);
console.log(keep(subArr) instanceof Vec, keep(plainArr) instanceof Vec, keep(plainArr).join(","));

// An indexed read in a counted loop is the other fast path a subclass must not
// be mistaken for a plain array on.
function total(xs) {
  let t = 0;
  for (let i = 0; i < xs.length; i++) t += xs[i];
  return t;
}
let hotTotal = 0;
for (let k = 0; k < 2000; k++) hotTotal = total(plainArr);
console.log(hotTotal, total(subArr), total(v));

// 23.1.3 puts `Array.prototype` itself among the array exotic objects: its own
// `length` is 0 and `Array.isArray` answers true for it (23.1.2.2 asks
// IsArray, which is about the exotic KIND and not about the prototype chain).
console.log(Array.prototype.length, Array.isArray(Array.prototype));
console.log(Array.isArray([]), Array.isArray({}), Array.isArray(v));

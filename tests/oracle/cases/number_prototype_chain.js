// `Number.prototype` as a holder: the members a number reaches THROUGH it, the
// Number exotic object, and the conversion form of the constructor.
//
// `cases/get_prototype_of_number` pins that the chain exists. This pins what
// hangs on it, and the difference matters because the two are not the same
// claim: a member table beside the value can answer `(1).toFixed(2)` perfectly
// and still leave `Number.prototype.toFixed` unreachable, `.call` with a
// detached method impossible, and `for (k in 5)` unanswerable. Every line below
// needs the OBJECT.
//
// From ECMA-262 21.1.1.1 (the constructor), 21.1.2 (its own properties),
// 21.1.3 (the prototype object), 20.1.2.12 (getPrototypeOf), 20.1.3.2
// (hasOwnProperty), 20.1.3.3 (isPrototypeOf) and 20.1.3.6 (toString):
//
// 1. The prototype is one object, reached identically from every number, from
//    a wrapper, and from the constructor's `prototype`.
// 2. A member read twice is the SAME function object, whichever route reaches
//    it — which is the whole content of "this is a holder".
// 3. Every member of 21.1.3 is non-enumerable, so a for-in over a number
//    visits nothing. That attribute is what made it safe to put a new link
//    under every number in a suite of pinned bytes.
// 4. thisNumberValue accepts a primitive OR a Number object, and refuses
//    anything else with the TypeError 21.1.3 names.
// 5. `Number(x)` is a conversion and `new Number(x)` is an object, and the two
//    are the same step 1 with different step 3s.

const p = Number.prototype;

// 21.1.3 and 21.1.3.1: the object, its own [[Prototype]], and its back-pointer.
console.log(Object.getPrototypeOf(p) === Object.prototype, p.constructor === Number);
console.log(Object.getPrototypeOf(0) === p, Object.getPrototypeOf(-1.5) === p);
console.log(Object.getPrototypeOf(NaN) === p, Number.prototype === p);

// The identity property. A fresh function object per read would fail every one
// of these while answering every call correctly.
console.log((5).toFixed === (7).toFixed, p.toFixed === (5).toFixed);
console.log(p.valueOf === (0).valueOf, p.toString === (0).toString);

// 21.1.3: every member non-enumerable, so nothing new appears in a for-in.
let seen = [];
for (const k in 5) seen.push(k);
console.log(seen.length, Object.keys(p).length);

// A member DETACHED from its receiver and applied with `.call`, which needs
// somewhere to detach it from. The digits are 21.1.3.3's and 21.1.3.6's, on the
// exact real number the double denotes — `cases/number_methods` derives them.
console.log(p.toFixed.call(1.005, 2), p.toString.call(255, 16), p.valueOf.call(42));

// thisNumberValue: a receiver that is neither a number nor a Number object.
try {
  p.toFixed.call("5", 2);
} catch (e) {
  console.log("detached toFixed throws:", e instanceof TypeError);
}

// The chain continues to `Object.prototype`. `hasOwnProperty` is false because
// ToObject(5) is a Number object with no own property at all; `isPrototypeOf`
// is false for a different reason — 20.1.3.3 step 1 returns false when V is not
// an Object, so no intrinsic is the prototype OF a primitive.
console.log(typeof (5).hasOwnProperty, (5).hasOwnProperty("toFixed"));
console.log(Object.prototype.isPrototypeOf(5), p.isPrototypeOf(5));

// 21.1.1.1 with NewTarget: the Number exotic object.
const w = new Number(7.5);
console.log(typeof w, Object.getPrototypeOf(w) === p, w instanceof Number);
console.log(w.valueOf(), w.toFixed(1), w + 1, w == 7.5, w === 7.5);
console.log(new Number(1), new Number(1.5));

// 20.1.3.6 step 9: the tag is [[NumberData]], and `Number.prototype` has one —
// 21.1.3 makes it a Number object whose slot is +0.
const ts = Object.prototype.toString;
console.log(ts.call(1), ts.call(w), ts.call(p));

// 21.1.1.1 without NewTarget is ToNumeric and produces a primitive. No argument
// at all is +0, which is NOT `Number(undefined)`.
console.log(Number("42"), Number(""), Number("x"), Number(true), Number(null), Number(undefined));
console.log(Number(), typeof Number(5), Number("0x1f"), Number(" 12 "));

// 21.1.2: all fifteen own properties of `Number` are non-enumerable, so
// `Object.keys` reports none — and they are still OWN properties, so `in`
// finds them.
console.log(Object.keys(Number).length, "MAX_SAFE_INTEGER" in Number, "prototype" in Number);
console.log(typeof Number, Number.isInteger(5), Number.MAX_SAFE_INTEGER);

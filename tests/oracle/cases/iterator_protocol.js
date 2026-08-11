// The iterator protocol (docs/0021 decision 2). `for-of` was an INDEX WALK
// over a `length` (docs/0012 decision 3), which is a correct answer only for
// an array and a string; every value below has no length at all and is
// iterated by calling `next()` until it says `done`.
//
// From ECMA-262 7.4.2 (GetIterator), 7.4.6 (IteratorStep), 27.1.2
// (%IteratorPrototype%) and 14.7.5.6 (ForIn/OfBodyEvaluation):
//
// 1. A user object is iterable because it has an `@@iterator` method, and
//    every construct that consumes an iterable goes through the same one:
//    `for-of`, spread, and array destructuring — including a rest element,
//    which drains what the elements before it left.
// 2. The method is looked up on the object, so it can live on a PROTOTYPE:
//    an instance of a class whose prototype carries it is iterable, and each
//    iteration gets a FRESH iterator, which is what makes a nested loop over
//    the same iterable terminate rather than share a cursor.
// 3. Each of the four ways the protocol can be malformed is its own named
//    TypeError, not one generic message: not iterable at all, an
//    `@@iterator` that returns a non-object, an iterator with no `next`, and
//    a `next` that returns a non-object.
// 4. An exception from `next` propagates unchanged — 7.4.6 leaves the
//    iterator alone, and `return` is NOT called on it.
//
// DELIBERATE DIVERGENCE, docs/0021 decision 1: bronze has no symbol
// primitive, so `Symbol.iterator` is the well-known STRING key "@@iterator"
// and `typeof Symbol.iterator` answers "string" where node answers "symbol".
// The compensating rule is pinned below: a key that begins with `@@` is
// created non-enumerable, so it stays out of `Object.keys` and `for-in`
// exactly as a real symbol key would. `cases/blocked/symbols.js` is the case
// for the primitive itself.

const range = {
  from: 1,
  to: 4,
  [Symbol.iterator]: function () {
    let i = this.from;
    const last = this.to;
    return {
      next: function () {
        if (i <= last) {
          const v = i;
          i = i + 1;
          return { value: v, done: false };
        }
        return { value: undefined, done: true };
      },
    };
  },
};

let sum = 0;
for (const v of range) sum = sum + v;
console.log(sum);
console.log([...range].join("-"));
const [a, b, ...rest] = range;
console.log(a, b, rest.join(","));

// The well-known key is a string, and it is hidden from enumeration.
console.log(typeof Symbol.iterator);
console.log(Object.keys(range).join(","));
for (const k in range) console.log("in:" + k);

// ---- the method on a prototype, and a fresh iterator per iteration --------
class Deck {
  constructor(n) {
    this.n = n;
  }
}
Deck.prototype[Symbol.iterator] = function () {
  let i = 0;
  const n = this.n;
  return {
    next: function () {
      i = i + 1;
      return i <= n ? { value: "c" + i, done: false } : { value: undefined, done: true };
    },
  };
};

const d = new Deck(3);
console.log([...d].join(","));
let twice = "";
for (const c of d) for (const e of d) twice = twice + c + e + ";";
console.log(twice);

// ---- every way the protocol can be malformed ------------------------------
try {
  for (const x of { a: 1 }) console.log(x);
} catch (e) {
  console.log(e.message);
}
try {
  for (const x of 5) console.log(x);
} catch (e) {
  console.log(e.message);
}
try {
  for (const x of null) console.log(x);
} catch (e) {
  console.log(e.message);
}
try {
  const bad = { [Symbol.iterator]: function () { return 1; } };
  for (const x of bad) console.log(x);
} catch (e) {
  console.log(e.message);
}
try {
  const bad2 = { [Symbol.iterator]: function () { return {}; } };
  for (const x of bad2) console.log(x);
} catch (e) {
  console.log(e.message);
}
try {
  const bad3 = { [Symbol.iterator]: function () { return { next: function () { return 7; } }; } };
  for (const x of bad3) console.log(x);
} catch (e) {
  console.log(e.message);
}
try {
  const boom = {
    [Symbol.iterator]: function () {
      return { next: function () { throw new Error("next threw"); } };
    },
  };
  for (const x of boom) console.log(x);
} catch (e) {
  console.log(e.message);
}

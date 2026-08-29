// `Object.assign` into a target that refuses the write.
//
// ECMA-262 20.1.2.1 step 5.c.ii.2 spells the copy `Set(to, nextKey,
// propValue, true)`. The `true` is the THROW flag of 7.3.4 Set, and it is a
// constant in the algorithm — not the strictness of whoever called
// `Object.assign`. So a refused copy is a TypeError out of sloppy code exactly
// as it is out of strict code, which is the one thing that separates this
// operation from the plain assignment `to.k = v` that would have been
// discarded.
//
// The file is sloppy on purpose. A strict file would prove nothing here: every
// refusal below would be a TypeError anyway, and the case would pass on a
// build that had simply threaded the CALLER's strictness through, which is the
// wrong rule.
//
// `Object.assign` also stops at the first refusal — it is a plain `Set` whose
// abrupt completion propagates — so a target that refuses its first key never
// sees the second, and the sources after it are never read.
//
// What each line pins:
//
// 1. A frozen target refuses an EXISTING key: 10.1.9.2 step 2.a, the property
//    being non-writable.
// 2. A frozen target refuses a NEW key: step 2.c.ii through 10.1.6.3 step 2.b,
//    the object not being extensible. A different refusal, and it has to be
//    reached even though the object has no such property to be non-writable.
// 3. A SEALED target takes an existing key: seal leaves data properties
//    writable, so this is not a refusal at all and must not throw. The throw
//    flag decides what happens to a refusal, not whether there is one.
// 4. A sealed target refuses a new key, for the same extensibility reason.
// 5. `Object.preventExtensions` alone: existing keys land, new ones refuse.
// 6. A target with a non-writable property made by `Object.defineProperty`
//    rather than by `freeze` — the same refusal from the other direction.
// 7. A target whose property is an accessor with no setter: 10.1.9.2 step 3.b,
//    the third of the three ways a Set answers false.
// 8. An empty source copies nothing, so a frozen target is untouched and
//    nothing throws. The refusal belongs to the WRITE and not to the target.
// 9. `null` and `undefined` sources are skipped (step 5.a), so they do not
//    reach a write either.
// 10. The copy stops at the first refusal: the second source is never read,
//    which is visible because reading it would have run a getter.
// 11. The target is returned unchanged by every call that did not throw.

function attempt(label, fn) {
  try {
    fn();
    console.log(label, "no throw");
  } catch (e) {
    console.log(label, e instanceof TypeError, e.name);
  }
}

const frozen = Object.freeze({ a: 1 });
attempt("frozen-existing", function () {
  Object.assign(frozen, { a: 5 });
});
attempt("frozen-new", function () {
  Object.assign(frozen, { b: 5 });
});
console.log(frozen.a, frozen.b);

const sealed = Object.seal({ a: 1 });
attempt("sealed-existing", function () {
  Object.assign(sealed, { a: 5 });
});
attempt("sealed-new", function () {
  Object.assign(sealed, { b: 5 });
});
console.log(sealed.a, sealed.b);

const closed = Object.preventExtensions({ a: 1 });
attempt("closed-existing", function () {
  Object.assign(closed, { a: 5 });
});
attempt("closed-new", function () {
  Object.assign(closed, { b: 5 });
});
console.log(closed.a, closed.b);

const readOnly = {};
Object.defineProperty(readOnly, "k", {
  value: 1,
  writable: false,
  enumerable: true,
  configurable: true,
});
attempt("nonwritable", function () {
  Object.assign(readOnly, { k: 5 });
});
console.log(readOnly.k);

const getterOnly = {
  get g() {
    return 3;
  },
};
attempt("no-setter", function () {
  Object.assign(getterOnly, { g: 5 });
});
console.log(getterOnly.g);

attempt("empty-source", function () {
  Object.assign(frozen, {});
});
attempt("nullish-sources", function () {
  Object.assign(frozen, null, undefined);
});
attempt("no-source", function () {
  Object.assign(frozen);
});

let read = 0;
const counted = {
  get later() {
    read += 1;
    return 1;
  },
};
attempt("stops-at-first", function () {
  Object.assign(frozen, { a: 5 }, counted);
});
console.log("second source read", read);

const open = { a: 1 };
console.log(Object.assign(open, { b: 2 }) === open, open.a, open.b);

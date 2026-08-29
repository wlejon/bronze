// Replacing a prototype on an object that is not extensible.
//
// ECMA-262 10.1.2.1 OrdinarySetPrototypeOf runs three tests in an order that
// matters. Step 2 is SameValue against the prototype the object ALREADY has,
// and it returns TRUE — a write that changes nothing succeeds whatever the
// object's state, which is why the defensive
// `Object.setPrototypeOf(x, Object.getPrototypeOf(x))` is never an error. Only
// then does step 3 read [[Extensible]], and step 4 returns false for an object
// that has been closed. 20.1.2.21 step 4 turns that false into a TypeError,
// and Annex B's `__proto__` setter reaches the same algorithm through
// B.2.2.1.2 step 5.
//
// The file is sloppy: 20.1.2.21 throws on its own, not because of the caller.
//
// A silently-succeeding version of this is worse than an ordinary wrong
// answer, because it leaves the object self-contradicting — the chain moved
// and `Object.isExtensible` goes on reporting false about it, so a program
// that closed an object to fix its shape can have the shape changed underneath
// it with every guard it wrote still answering correctly.
//
// What each line pins:
//
// 1. `preventExtensions` then a DIFFERENT prototype: TypeError, and the chain
//    does not move.
// 2. The same object and the prototype it already has: no throw, because step
//    2 answers before [[Extensible]] is ever read.
// 3. `seal` and `freeze` reach it too — both set [[Extensible]] false — and
//    `null` is a different prototype like any other, so closing an object
//    forbids cutting its chain as much as replacing it.
// 4. An ordinary extensible object still takes a new prototype, and the
//    inherited property is visible through it. The refusal is about
//    [[Extensible]] and not about the operation.
// 5. `obj.__proto__ = p` answers exactly the same way, because B.2.2.1.2 step
//    5 IS [[SetPrototypeOf]]. Two spellings, one operation — a build where
//    only one of them refused would be a hole a program falls into by
//    changing how it spells the write.
// 6. `__proto__` with a non-object, non-null value is a quiet return
//    (B.2.2.1.2 step 2), even on a closed object: there is no
//    [[SetPrototypeOf]] to refuse, since the setter never reaches step 5.
// 7. `Object.setPrototypeOf` returns its first argument, so it composes; that
//    is only observable on the calls that did not throw.

function attempt(label, fn) {
  try {
    fn();
    console.log(label, "no throw");
  } catch (e) {
    console.log(label, e instanceof TypeError, e.name);
  }
}

const donor = { q: 7 };

const closed = Object.preventExtensions({ a: 1 });
attempt("closed-different", function () {
  Object.setPrototypeOf(closed, donor);
});
console.log(closed.q, Object.getPrototypeOf(closed) === Object.prototype);

attempt("closed-same", function () {
  Object.setPrototypeOf(closed, Object.prototype);
});
console.log(Object.getPrototypeOf(closed) === Object.prototype);

const sealed = Object.seal({ a: 1 });
attempt("sealed-different", function () {
  Object.setPrototypeOf(sealed, donor);
});
console.log(sealed.q);

const frozen = Object.freeze({ a: 1 });
attempt("frozen-null", function () {
  Object.setPrototypeOf(frozen, null);
});
console.log(Object.getPrototypeOf(frozen) === Object.prototype);

const open = { a: 1 };
attempt("open-different", function () {
  Object.setPrototypeOf(open, donor);
});
console.log(open.q, Object.getPrototypeOf(open) === donor);

const closedProto = Object.preventExtensions({ a: 1 });
attempt("proto-setter-different", function () {
  closedProto.__proto__ = donor;
});
console.log(closedProto.q, Object.getPrototypeOf(closedProto) === Object.prototype);

attempt("proto-setter-same", function () {
  closedProto.__proto__ = Object.prototype;
});

attempt("proto-setter-primitive", function () {
  closedProto.__proto__ = 5;
});
console.log(Object.getPrototypeOf(closedProto) === Object.prototype);

const openProto = { a: 1 };
attempt("proto-setter-open", function () {
  openProto.__proto__ = donor;
});
console.log(openProto.q);

console.log(Object.setPrototypeOf(open, donor) === open);

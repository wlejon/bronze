// `Reflect.set` answers the boolean that [[Set]] returned.
//
// ECMA-262 28.1.13 step 4 is `return ? target.[[Set]](key, V, receiver)`, and
// 10.1.9 [[Set]] is a predicate: it answers false for the three refusals
// 10.1.9.2 defines and true otherwise. That is the whole reason the member
// exists — the assignment operator already performs the write, and what it
// cannot do is TELL you whether the write happened. So a `Reflect.set` that
// answers true about a store that did not land is not a small error; it is the
// operation returning the opposite of its only content, and it does so
// silently, which is how a program built on it (a validating proxy, a
// copy-on-write wrapper) draws a wrong conclusion from a correct-looking API.
//
// `Reflect.set` never throws for a refusal — that is what separates it from
// `Object.assign` and from a strict assignment — so this case is sloppy and
// would read the same in strict code.
//
// Three arguments and four are ONE algorithm: 28.1.13 step 3 says an absent
// receiver IS the target. So every line below is run both ways and the two
// answers must agree wherever the receiver is the target.
//
// What each line pins:
//
// 1. A frozen object refuses an existing key (non-writable) and a new key
//    (non-extensible): both false, and neither stores.
// 2. A sealed object takes an existing key and refuses a new one — the two
//    refusals are different tests and only the second applies.
// 3. An accessor with no setter is false (10.1.9.2 step 3.b), and one WITH a
//    setter is true and runs it.
// 4. A frozen array refuses an element and its `length`; a sealed array takes
//    an element. An array keeps its integrity level somewhere else entirely
//    from a plain object, and the answer must not depend on that.
// 5. An inherited non-writable data property refuses a SHADOWING write (step
//    2.a reaches the parent's descriptor), while an inherited writable one is
//    shadowed and answers true.
// 6. A distinct receiver: the write lands on the RECEIVER, not on the target,
//    and the receiver's own state is what refuses it.
// 7. An ordinary write answers true, so the predicate is not simply pessimistic.

const frozen = Object.freeze({ a: 1 });
console.log(Reflect.set(frozen, "a", 9), frozen.a);
console.log(Reflect.set(frozen, "b", 9), frozen.b);
console.log(Reflect.set(frozen, "a", 9, frozen), frozen.a);

const sealed = Object.seal({ a: 1 });
console.log(Reflect.set(sealed, "a", 9), sealed.a);
console.log(Reflect.set(sealed, "b", 9), sealed.b);

const getterOnly = {
  get g() {
    return 3;
  },
};
console.log(Reflect.set(getterOnly, "g", 9), getterOnly.g);

const withSetter = {
  set s(v) {
    this.seen = v;
  },
};
console.log(Reflect.set(withSetter, "s", 9), withSetter.seen);

const frozenArray = Object.freeze([1, 2]);
console.log(Reflect.set(frozenArray, 0, 9), frozenArray[0]);
console.log(Reflect.set(frozenArray, "length", 0), frozenArray.length);
console.log(Reflect.set(frozenArray, 5, 9), frozenArray.length);

const sealedArray = Object.seal([1, 2]);
console.log(Reflect.set(sealedArray, 0, 9), sealedArray[0]);
console.log(Reflect.set(sealedArray, 5, 9), sealedArray.length);

const lockedBase = Object.freeze({ k: 1 });
const heir = Object.create(lockedBase);
console.log(Reflect.set(heir, "k", 9), heir.k);
console.log(Object.prototype.hasOwnProperty.call(heir, "k"));

const openBase = { k: 1 };
const heir2 = Object.create(openBase);
console.log(Reflect.set(heir2, "k", 9), heir2.k, openBase.k);

const target = { a: 1 };
const closedReceiver = Object.preventExtensions({});
console.log(Reflect.set(target, "a", 9, closedReceiver), target.a, closedReceiver.a);

const openReceiver = {};
console.log(Reflect.set(target, "a", 8, openReceiver), target.a, openReceiver.a);

const plain = { a: 1 };
console.log(Reflect.set(plain, "a", 9), plain.a);
console.log(Reflect.set(plain, "b", 9), plain.b);

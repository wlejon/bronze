// Property descriptors: the two attributes a bronze property gained in
// docs/0021 decision 5, `writable` and `configurable`, and the `extensible`
// bit `Object.freeze` sets. All three live in the DICTIONARY rather than in
// the shape transition key, so an object with a non-default attribute has a
// private shape no inline cache can ever match.
//
// Not pinned here, deliberately: redefining a non-configurable property, and
// `defineProperty` on a frozen object, are both TypeErrors, and they belong
// with the other throwing cases. Every line below is a sloppy-mode SILENT
// outcome, which is what makes it checkable without a `try`.
//
// From ECMA-262 6.2.6 (property descriptors), 10.1.6.3, 7.3.5, 20.1.2.6 and
// 20.1.2.10:
//
// 1. `defineProperty` can create a property that reads back normally but is
//    invisible to `Object.keys` and to `for-in`, cannot be assigned over, and
//    cannot be deleted — the four attributes, each observable on its own.
// 2. An OMITTED attribute defaults to `false`, which is the trap that makes
//    `defineProperty` different from assignment: `{ value: 5 }` produces a
//    non-enumerable, non-writable, non-configurable property, where `o.a = 5`
//    produces one that is all three.
// 3. `getOwnPropertyDescriptor` reports those attributes as a fresh object
//    with the field order 6.2.6.4 fixes — value, writable, enumerable,
//    configurable for a data property; get, set, enumerable, configurable for
//    an accessor — and answers `undefined` for a key that is not an own one.
// 4. An accessor defined through `defineProperty` behaves exactly like one
//    written as `get x()`: the getter runs with the receiver as `this`, and
//    `delete` removes the pair (docs/0019 decisions 3 and 4). This is the
//    line that proves descriptors reached the same machinery rather than a
//    parallel one.
// 5. `Object.freeze` is the everyday name for all of it: existing properties
//    stop being writable and configurable, new ones stop being addable, and
//    every one of those failures is silent outside strict mode.
const locked = {};
Object.defineProperty(locked, "hidden", {
  value: 1,
  writable: false,
  enumerable: false,
  configurable: false,
});
console.log(locked.hidden);
console.log(Object.keys(locked).length);
let seen = "";
for (const k in locked) {
  seen = seen + k;
}
console.log("[" + seen + "]");
locked.hidden = 99;
console.log(locked.hidden);
console.log(delete locked.hidden);
console.log(locked.hidden);

const defaults = {};
Object.defineProperty(defaults, "a", { value: 5 });
const assigned = {};
assigned.a = 5;
console.log(defaults.a);
console.log(assigned.a);
console.log(Object.keys(defaults).length);
console.log(Object.keys(assigned).length);
defaults.a = 6;
assigned.a = 6;
console.log(defaults.a);
console.log(assigned.a);

const dd = Object.getOwnPropertyDescriptor(assigned, "a");
console.log(dd.value);
console.log(dd.writable);
console.log(dd.enumerable);
console.log(dd.configurable);
console.log(Object.keys(dd).join(","));

const lit = {
  get x() {
    return 7;
  },
};
const ad = Object.getOwnPropertyDescriptor(lit, "x");
console.log(typeof ad.get);
console.log(ad.set);
console.log(ad.enumerable);
console.log(ad.configurable);
console.log(Object.keys(ad).join(","));
console.log(Object.getOwnPropertyDescriptor(lit, "nope"));

const computed = { n: 1 };
Object.defineProperty(computed, "twice", {
  get: function () {
    return this.n * 2;
  },
  enumerable: true,
  configurable: true,
});
console.log(computed.twice);
computed.n = 4;
console.log(computed.twice);
console.log(Object.keys(computed).join(","));
console.log(delete computed.twice);
console.log(computed.twice);

const frozen = Object.freeze({ a: 1, b: 2 });
frozen.a = 3;
frozen.c = 4;
console.log(frozen.a);
console.log(frozen.c);
console.log(delete frozen.b);
console.log(Object.isFrozen(frozen));
console.log(Object.keys(frozen).join(","));

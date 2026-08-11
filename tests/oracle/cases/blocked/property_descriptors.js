// BLOCKED: `unsupported builtin: Object.defineProperty`, and the same for
// `Object.getOwnPropertyDescriptor`, `Object.freeze` and `Object.isFrozen`.
// docs/0011 decision 1 keeps an unbuilt member a named error rather than a
// stub, so the diagnosis is working — but this is now the largest remaining
// hole in the property model, and it is the direct successor to the chunk
// that added accessors.
//
// The blocker is that a bronze property has exactly TWO attributes. A shape
// node records the key, whether it is enumerable, and whether it is an
// accessor pair (docs/0019 decision 1); `writable` and `configurable` are
// deliberately absent, which is why `delete` in bronze can never answer
// `false` and why a write can never be silently discarded for any reason
// other than a missing setter. Landing descriptors means all four of:
//
//  - Two more bits in the shape TRANSITION KEY. A transition is matched on
//    (name, enumerable, accessor) today; adding two more attributes doubles
//    the ways two objects that "have the same properties" can fail to share a
//    shape, and a `writable: false` property added late would fork the tree
//    for every object that had reached that point. That is a shape-tree cost
//    question, not a syntax one.
//  - A `[[DefineOwnProperty]]` that is genuinely separate from `[[Set]]`.
//    The `defineOwn` flag added for `method.def` is a first step, but it says
//    only "do not run an inherited setter"; a real DefineOwnProperty has to
//    validate the incoming descriptor against the existing one (6.2.6.6).
//  - An `extensible` bit per object, which is what `Object.freeze` and
//    `Object.preventExtensions` actually set, and which the inline caches
//    have to treat the way they already treat dictionary mode: a frozen
//    object's writes must not be folded into a cached slot store.
//  - A descriptor OBJECT round trip. `getOwnPropertyDescriptor` builds a
//    fresh object whose field order is fixed by 6.2.6.4, and
//    `defineProperty` reads one back with every field optional.
//
// Not pinned here, deliberately: redefining a non-configurable property, and
// `defineProperty` on a frozen object, are both TypeErrors, and bronze has no
// `throw` yet (`try_catch_throw` in this directory is the case for that).
// Every line below is a sloppy-mode SILENT outcome, which is what makes it
// checkable today.
//
// What this case pins when it lands, from ECMA-262 6.2.6 (property
// descriptors), 10.1.6.3, 7.3.5, 20.1.2.6 and 20.1.2.10:
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

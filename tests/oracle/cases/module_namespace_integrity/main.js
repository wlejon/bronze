// A module namespace's INTEGRITY STATE, and the two enumerations that read its
// own properties out — 10.4.6.3, 7.3.14, 7.3.15, 25.5.2 and 7.3.25.
//
// Every byte of `main.expected` was derived by hand from ECMA-262 before this
// program was compiled. The three predicates are the interesting part, because
// two of the three answers are counter-intuitive and neither is a bronze
// shortcut:
//
//   - [[IsExtensible]] (10.4.6.3) is "return false". Not a slot, not a
//     condition, not something `preventExtensions` has to have been called for:
//     a namespace is non-extensible from birth, which is why bronze needs
//     nowhere to record it.
//   - so `isSealed` is TRUE with nothing having been done to the object —
//     7.3.15 wants non-extensible plus every own property non-configurable, and
//     10.4.6.5 reports `configurable: false` for every export.
//   - and `isFrozen` is FALSE for the same object at the same moment, because
//     frozen additionally wants every own DATA property non-writable and
//     10.4.6.5 reports `writable: true`. That is not a contradiction: the
//     exporting module may still assign to the binding — `bump()` does below —
//     and 6.1.7.3 forbids a non-writable non-configurable property whose value
//     changes.
//
// `seal` and `freeze` then split for exactly that reason. Both run 7.3.14,
// whose [[PreventExtensions]] step succeeds (10.4.6.4 returns true, having
// nothing to do); the difference is the descriptor step 5 defines. `seal` asks
// for `configurable: false`, which MATCHES what 10.4.6.5 already reports, and
// 10.4.6.6 accepts a matching descriptor — so it succeeds as a no-op. `freeze`
// asks for `writable: false`, which does not match, so 10.4.6.6 returns false
// and DefinePropertyOrThrow throws. A TypeError is the language's answer here,
// not bronze declining to have one.
import * as ns from './lib.js';
import * as empty from './empty.js';

console.log(Object.isExtensible(ns), Object.isSealed(ns), Object.isFrozen(ns));

// 7.3.14 returns the object, and nothing about it changed — there was nothing
// for either operation to change.
console.log(Object.seal(ns) === ns, Object.preventExtensions(ns) === ns);
console.log(Object.isExtensible(ns), Object.isSealed(ns), Object.isFrozen(ns));

try {
  Object.freeze(ns);
  console.log('no throw');
} catch (e) {
  console.log(e instanceof TypeError);
}
// The failed freeze left the object exactly as it was — a thrown
// SetIntegrityLevel is not a partial one here, because there was no state to
// half-write.
console.log(Object.isSealed(ns), Object.isFrozen(ns));

// The empty namespace is the other side of that split: no own property means
// nothing for the `writable: false` step to reach, so 7.3.14 completes and
// 7.3.15's frozen test passes vacuously.
console.log(Object.keys(empty).length, Object.isSealed(empty), Object.isFrozen(empty));
console.log(Object.freeze(empty) === empty, Object.isFrozen(empty));

// ---- the two enumerations ---------------------------------------------------
// 25.5.2.4 asks for EnumerableOwnPropertyNames, and 10.4.6.5 makes every export
// enumerable — so a namespace serializes as its exports in 10.4.6.2's sorted
// order, with the callable one omitted by 25.5.2.3 exactly as a function-valued
// property of any object is. `{}` would be a wrong answer and not a missing
// one.
console.log(JSON.stringify(ns));
console.log(JSON.stringify(ns, null, 2));

// 7.3.25 CopyDataProperties, which takes the same own keys and reads each one
// through [[Get]]. The copy is a plain object, so its key order is the order
// they were copied in, which is the namespace's sorted order.
const copy = { ...ns };
const assigned = Object.assign({}, ns);
console.log(Object.keys(copy).join(','), copy.c, copy.k, typeof copy.bump);
console.log(Object.keys(assigned).join(','), assigned.c, assigned.k);

// The namespace is a live view and the copies are values, which is what makes
// the two answers below differ after the export moves.
ns.bump();
console.log(ns.k, copy.k, assigned.k);
console.log(JSON.stringify(ns));

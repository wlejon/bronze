// Blocked: `cannot assign to 'ns.z': it is a binding of the imported module
// namespace ... which is read-only`, and — even without that line — a key
// order bronze does not produce.
//
// A module namespace is an exotic object (ECMA-262 10.4.6). Three facts about
// it that bronze's stand-in (an object literal of getters, docs/0023 decision
// 4) does not have:
//
//   - 10.4.6.2 OwnPropertyKeys returns the export names SORTED by code unit,
//     so `z` declared first still comes back after `a`;
//   - every property is non-writable and non-configurable, and module code is
//     always strict, so `ns.z = 5` is a TypeError rather than a no-op;
//   - the write above is refused at COMPILE time by bronze today, which is why
//     this case does not build at all rather than printing three wrong lines.
import * as ns from './lib.js';

console.log(Object.keys(ns).join(','));
try {
  ns.z = 5;
  console.log('no throw');
} catch (e) {
  console.log('TypeError');
}
console.log(ns.z);

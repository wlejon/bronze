// A module namespace is an exotic object (ECMA-262 10.4.6), and these three
// lines are the three things about one that an object literal of getters is
// not:
//
//   - 10.4.6.2 OwnPropertyKeys returns the export names SORTED by code unit, so
//     `z` — declared first in lib.js — still comes back after `a`;
//   - 10.4.6.9 [[Set]] returns false for every key, and module code is always
//     strict (11.2.2), so `ns.z = 5` is a TypeError rather than a no-op;
//   - it is a RUNTIME TypeError and not a compile error, because it assigns to
//     a property of an ordinary object VALUE. `import { z } from './lib.js';
//     z = 5` is the other operation — that one assigns to the immutable import
//     BINDING and is refused when the graph is linked. Conflating the two is
//     what kept this program from building at all.
//
// The refused write leaves the export where it was, which is the last line.
import * as ns from './lib.js';

console.log(Object.keys(ns).join(','));
try {
  ns.z = 5;
  console.log('no throw');
} catch (e) {
  console.log('TypeError');
}
console.log(ns.z);

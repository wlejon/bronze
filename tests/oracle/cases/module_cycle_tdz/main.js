// The other half of cases/module_cycle: a cycle whose crossing binding is NOT
// a hoisted function declaration.
//
// b.js is the end of the cycle the loader leaves first, so its body runs
// first, and `marker` — a.js's `const` — has been created and not initialized
// when it does. ECMA-262 16.2.1.6.4 instantiates a module's lexical bindings
// uninitialized before any body in the graph runs, so that read is 9.1.1.1.6's
// ReferenceError. It must NOT be `undefined`: answering it would be the silent
// wrong answer that made cycles worth refusing in the first place.
//
// The same binding read AFTER the whole graph has been evaluated answers, and
// that is the point of pinning both in one file: the dead zone is a moment,
// not a module boundary, and a cycle is well defined exactly where every
// crossing read happens after the declaration it names.
import { report } from './a.js';

console.log(report());

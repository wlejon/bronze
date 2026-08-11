// An import binding is a LIVE VIEW of the exporting module's binding, not a
// copy taken when the module was linked (ECMA-262 9.4.2 CreateImportBinding /
// 16.2.1.6.4: an import creates an *indirect* binding into the exporting
// module's environment record, and reading it reads that record now).
//
// This is the case a copy-at-link-time implementation fails and nothing else
// catches: every other module test passes with copies. `count` is mutated
// only from inside counter.js, and read here and from a third module.
import { count, bump, frozen } from './counter.js';
import { observed } from './observer.js';

console.log(count, observed());
bump();
console.log(count, observed());
bump();
bump();
console.log(count, observed());

// The control: a const initialised from `count` before any mutation, so it
// stays 0 however many times `bump` has run.
console.log(frozen);

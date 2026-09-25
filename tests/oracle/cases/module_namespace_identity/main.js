// One module has exactly one namespace object (ECMA-262 16.2.1.6.2
// GetModuleNamespace: created once, then returned from [[Namespace]]). Every
// way a unit can reach one — `import * as` in several files, `export * as`,
// `import()` with a string specifier, and the entry's own namespace reached
// through a cycle — must hold that same object.
import * as m from './m.js';
import * as self from './main.js';
import { nsA, entryNs } from './a.js';
import { nsB, reNs } from './b.js';

export const marker = 'entry';

console.log(m === nsA, m === nsB, nsA === nsB);
console.log(reNs === m);
console.log(self === entryNs(), self.marker);

// The identity is of the object, not of a snapshot: a later write in the
// exporting module shows through every name for it.
m.bump();
console.log(nsA.count, nsB.count, reNs.count);

import('./m.js').then((d) => {
  console.log('dynamic', d === m);
  return import('./a.js');
}).then((a) => console.log('dynamic a', a.nsA === m));

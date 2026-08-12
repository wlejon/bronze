// A diamond: main imports left and right, and both import base. base is read,
// parsed and evaluated exactly ONCE, before either of them, and the order of
// the whole graph is the post-order of a depth-first walk over the import
// declarations in source order.
import { L } from './left.js';
import { R } from './right.js';
import { inits } from './base.js';

console.log(L + R);
console.log(inits);

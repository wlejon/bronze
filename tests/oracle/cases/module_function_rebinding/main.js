import { ext, counter, bump, swapShape, shapeK } from './lib.js';
console.log(JSON.stringify(ext({ a: 1 }, { b: 2 })), JSON.stringify(ext({ c: 3 }, {})));
console.log(counter());
bump();
console.log(counter());
console.log(shapeK());
swapShape();
console.log(shapeK());
// A rebound declaration is initialized before any statement runs, and keeps
// its name.
console.log(early(), early.name, typeof early.prototype);
function early() { early = function () { return 'second'; }; return 'first'; }
console.log(early(), early.name);
function local() { return 'a'; }
local = () => 'b';
console.log(local());

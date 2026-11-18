// Sloppy code that assigns a name no scope declares creates a property of the
// global object (9.1.1.4.5 SetMutableBinding on the global environment), at
// the top level and inside a function: HexGL's `Leap = require(...)` and
// Astray's `wh = window.innerHeight` do it. The property is an ordinary data
// property (configurable, so `delete` removes it), read back by any code
// through the global object. Strict code that writes a name the global object
// lacks is a ReferenceError (6.2.5.6 step 3); one it has is set. A compound,
// update or logical assignment READS first, so on a missing name it is the
// ReferenceError of the read in either mode.
console.log(typeof Later);
Top = 1;
(function () { Inner = 2; })();
console.log(Top, Inner, globalThis.Top, delete globalThis.Inner, typeof Inner);
Later = 'set';
console.log(Later, JSON.stringify(Object.getOwnPropertyDescriptor(globalThis, 'Later')));
try { Missing += 1; } catch (e) { console.log('compound', e.constructor.name); }
try { Missing2++; } catch (e) { console.log('update', e.constructor.name); }
try { Nope ??= 1; } catch (e) { console.log('logical', e.constructor.name); }
[G1, G2] = [1, 2];
({ a: G3 } = { a: 3 });
console.log(G1 + G2 + G3);
Cnt = 1; Cnt += 2; Cnt++; Lg = 0; Lg ||= 'lg';
console.log(Cnt, Lg);
try { (function () { 'use strict'; StrictNew = 1; })(); } catch (e) { console.log('strict', e.constructor.name, e.message); }
(function () { 'use strict'; Later = 'strict ok'; })();
console.log(Later, delete globalThis.Later, typeof Later);
function writer() { Shared = (typeof Shared === 'number' ? Shared : 0) + 1; }
writer(); writer();
console.log(Shared, globalThis.Shared);

// `undefined` is an identifier, not a reserved word: only the GLOBAL property
// is read-only. A parameter (jQuery's `(function (window, undefined) { … })`),
// a variable, a catch binding or a pattern may bind it, and a reference inside
// that scope is to the binding. At the top level a write to the global one,
// like one to NaN or Infinity, does nothing in sloppy code and is a TypeError
// in strict. Strict code may bind it too.
(function (window, undefined) {
  console.log(typeof window, undefined === void 0);
})(globalThis);
(function () { var undefined = 5; console.log(undefined); })();
(function () { let NaN = 1, Infinity = 2; console.log(NaN, Infinity); })();
try { throw 1; } catch (undefined) { console.log('catch', undefined); }
(function (eval, arguments) { console.log(eval, arguments); })(3, 4);
function f(undefined) { undefined = 3; return undefined; }
console.log(f(), f(1));
function g() { var r = typeof undefined; var undefined = 'x'; return r + ' ' + undefined; }
console.log(g());
const h = (undefined) => undefined === void 0;
console.log(h(), h(0));
function k({ undefined }) { return undefined; }
console.log(k({ undefined: 'pat' }));
console.log(JSON.stringify((function (undefined) { return { undefined }; })(4)), 'undefined' in { undefined });
undefined = 9; NaN = 1; Infinity = 2;
console.log(undefined, NaN, Infinity);
try { (function () { 'use strict'; undefined = 1; })(); } catch (e) { console.log(e.constructor.name); }
try { (function () { 'use strict'; NaN = 1; })(); } catch (e) { console.log(e.constructor.name); }
console.log((function (undefined) { 'use strict'; return undefined; })('strict param'));

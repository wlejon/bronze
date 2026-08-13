// ECMA-262 10.4.6.1: a module namespace object is created with one own
// SYMBOL-keyed property — `@@toStringTag`, holding the string "Module",
// { [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false }.
//
// It is the only own key of one that is not an export name, which is what makes
// it worth pinning from four directions at once: the tag 20.1.3.6 reads,
// the property itself, `in` (10.4.6.4), and the own-key lists — where it
// belongs to `getOwnPropertySymbols` (20.1.2.11) and to neither `Object.keys`
// nor `getOwnPropertyNames`, both of which report string keys alone.
//
// 10.4.6.2 [[OwnPropertyKeys]] sorts the exported names by code unit, which is
// why `one` comes back before `two` however the exporting module wrote them.
import * as lib from './lib.js';

const ts = Object.prototype.toString;
console.log(ts.call(lib));
console.log(lib[Symbol.toStringTag]);
console.log(Symbol.toStringTag in lib);

const syms = Object.getOwnPropertySymbols(lib);
console.log(syms.length);
console.log(syms[0] === Symbol.toStringTag);

const desc = Object.getOwnPropertyDescriptor(lib, Symbol.toStringTag);
console.log(desc.value);
console.log(desc.writable, desc.enumerable, desc.configurable);

console.log(Object.keys(lib).join(','));
console.log(Object.getOwnPropertyNames(lib).join(','));
console.log(Object.hasOwn(lib, 'one'), Object.hasOwn(lib, 'three'));

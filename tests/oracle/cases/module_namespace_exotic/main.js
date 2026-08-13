// Everything ECMA-262 10.4.6 says about a module namespace that
// `cases/module_namespace_object` does not reach: the whole of the sorted key
// list, the descriptor, the two answers for a name that is not an export, and
// the four other own-key members that have to agree with each other.
//
// The sort (10.4.6.2) is pinned over six names rather than two, because two
// alphabetical names pass under no sort at all. `writable: true` in the
// descriptor is 10.4.6.5's own answer and is not a slip: the exporting module
// may still assign to the binding — `bump()` below does — and 6.1.7.3 forbids a
// non-writable non-configurable property whose value changes. What refuses
// `ns.z = 5` is [[Set]] (10.4.6.9), which returns false whatever the descriptor
// says; they are different internal methods and only one of them is an
// attribute.
import * as ns from './lib.js';

console.log(Object.keys(ns).join(','));
// Every export is enumerable, so dropping the filter changes nothing.
console.log(Object.getOwnPropertyNames(ns).join(','));
// 10.4.6.1 fixes [[Prototype]] at null, so this is the language's answer and
// not "bronze has no prototype object for this kind".
console.log(Object.getPrototypeOf(ns));
console.log(typeof ns);
// 10.4.6.7 step 3: a name the module does not export reads `undefined`, exactly
// as an ordinary object does. `import { missing } from './lib.js'` is the early
// error; this is not one.
console.log(ns.missing);

const d = Object.getOwnPropertyDescriptor(ns, 'a');
console.log(d.value, d.writable, d.enumerable, d.configurable);
// 6.2.6.4 FromPropertyDescriptor's field order, which is pinned bytes here.
console.log(Object.keys(d).join(','));
console.log(Object.getOwnPropertyDescriptor(ns, 'missing'));

const seen = [];
for (const k in ns) seen.push(k);
console.log(seen.join(','));

console.log(Object.hasOwn(ns, 'z'), Object.hasOwn(ns, 'missing'));

// A live view: the namespace holds no values, so a write inside the exporting
// module is visible through it (10.4.6.7 reads the binding out of the module's
// environment record).
console.log(ns.z);
ns.bump();
console.log(ns.z);

try { ns.z = 5; } catch (e) { console.log(e instanceof TypeError); }
// 10.4.6.10 [[Delete]] answers false for an exported name — the property is
// non-configurable — and strict code turns that false into a TypeError.
try { delete ns.z; } catch (e) { console.log('delete ' + (e instanceof TypeError)); }
console.log(ns.z);

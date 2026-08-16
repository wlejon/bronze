// 16.2.1.10 `import.meta`, over a two-module graph.
//
// Nothing here pins an absolute path, and that is the point rather than a
// concession: the URL names the file the expression is written in, so its
// SUFFIX is the whole of what a machine-independent expectation can hold, and
// the identity questions are what the rest of the feature is.
//
// The object is OrdinaryObjectCreate(NULL) (step 1), so it has no prototype and
// every property but the host's reads as `undefined` — `toString` and
// `hasOwnProperty` included, which is what separates it from an object literal.
// It is ORDINARY and extensible, not frozen: step 4 hands it to the host to add
// properties to, and a program may add its own. That write is also how the
// identity is pinned across a function boundary — a second object would not
// carry it.
//
// `import.meta` is an EXPRESSION (13.3.12), not a ModuleItem, so it is legal
// inside a function body as well as at the top of a module; both are exercised.
import { depUrl, depSameTwice, depMetaTag } from './dep.js';

console.log(typeof import.meta, typeof import.meta.url);
console.log(import.meta.url.startsWith('file:///'), import.meta.url.endsWith('/main.js'));
console.log(import.meta === import.meta);
console.log(Object.getPrototypeOf(import.meta) === null);
console.log(Object.keys(import.meta).join(','));
console.log(import.meta.nothing === undefined, typeof import.meta.toString);
console.log(typeof import.meta.hasOwnProperty, typeof import.meta.valueOf);

console.log(depSameTwice, depUrl.startsWith('file:///'), depUrl.endsWith('/dep.js'));
console.log(depUrl === import.meta.url);

function nested() {
  return import.meta;
}
console.log(nested() === import.meta);

import.meta.tag = 'main';
console.log(nested().tag, depMetaTag());

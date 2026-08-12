// `import * as ns` binds one object whose properties are views of the
// exporting module's bindings — so `lib.n` after `lib.inc()` is the new value,
// not the one linking saw (ECMA-262 10.4.6.7: the namespace's [[Get]] reads
// the binding out of the module's environment record).
//
// What bronze's namespace object is NOT — a module namespace exotic object with
// sorted keys, non-writable properties and Symbol.toStringTag 'Module' — is
// `cases/blocked/module_namespace_object/`.
import * as lib from './lib.js';

console.log(lib.name);
console.log(lib.n);
lib.inc();
console.log(lib.n);
// `default` is an IdentifierName after a `.`, so this is the ordinary way to
// reach a default export through a namespace.
console.log(lib.default);
console.log(typeof lib);

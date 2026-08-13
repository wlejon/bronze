// `import * as ns` binds one object whose properties are views of the
// exporting module's bindings — so `lib.n` after `lib.inc()` is the new value,
// not the one linking saw (ECMA-262 10.4.6.7: the namespace's [[Get]] reads
// the binding out of the module's environment record).
//
// The rest of what 10.4.6 makes that object — sorted own keys, a [[Set]] that
// always refuses, a `configurable: false` descriptor, a null prototype — is
// `cases/module_namespace_object` and `cases/module_namespace_exotic`. What is
// still missing is `Symbol.toStringTag`, which 10.4.6.1 gives the value
// 'Module' and bronze does not carry.
import * as lib from './lib.js';

console.log(lib.name);
console.log(lib.n);
lib.inc();
console.log(lib.n);
// `default` is an IdentifierName after a `.`, so this is the ordinary way to
// reach a default export through a namespace.
console.log(lib.default);
console.log(typeof lib);

// Blocked: `unsupported module specifier "lib"`.
//
// A bare specifier is resolved by walking `node_modules` upward from the
// importing file, reading each candidate's `package.json` for `exports` or
// `main`, and guessing an extension or a directory index where the specifier
// names neither a file nor a mapping. Every one of those steps can pick a
// different file than the program meant, and picking wrong there is not a
// compile error — it is a different program — so bronze names the specifier
// rather than guessing. The package this needs is beside this file, already
// written, for whoever builds resolution.
import { helper } from 'lib';

console.log(helper());

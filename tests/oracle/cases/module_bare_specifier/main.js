// A BARE specifier. `lib` names a package, found by walking `node_modules`
// upward from this file — nearest wins — and read out of that package's
// `package.json`. The one beside this file says `"main": "index.js"`, so that
// is the file `helper` comes from.
//
// The walk is implemented because "the nearest `node_modules` wins" has exactly
// one answer. Every step of the real algorithm that has more than one is a hard
// error naming the ambiguity instead of a guess: a CONDITIONAL `exports` object
// (choosing a condition differently from the way the package was written
// resolves to another entry point and reports nothing), an `exports` pattern or
// fallback array, and any extension or directory-index guess — `"main": "./lib"`
// with both `lib.js` and `lib/index.js` present names both and takes neither.
// Picking wrong at any of them is not a compile error, it is a different
// program (src/modules/package.cpp).
import { helper } from 'lib';

console.log(helper());

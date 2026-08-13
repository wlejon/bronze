// The `node_modules` WALK, which the one-package case cannot show: two packages
// called `lib`, and which one an importer gets is decided by where the importer
// is. `main.js` sits beside the outer one; `sub/inner.js` sits beside the inner
// one, and the nearest wins.
//
// That rule is implemented rather than refused because it has exactly one
// answer — every tool agrees on it — which is the line the rest of package
// resolution is drawn against: a step with more than one answer is a named hard
// error instead (src/modules/package.cpp).
//
// The two packages also name their entry points differently, so both spellings
// bronze reads are pinned here: `"exports"` as a plain string, and `"main"`.
import { who } from 'lib';
import { seen } from './sub/inner.js';

console.log(who);
console.log(seen);

// Blocked: `cyclic module dependency: a.js -> b.js -> a.js`.
//
// a.js and b.js import each other, and every binding crossing the cycle is a
// hoisted `function` declaration — which ECMA-262 16.2.1.6.4 instantiates for
// the whole graph before any module body runs, so nothing here can observe an
// uninitialised binding and the program is well defined. bronze refuses every
// cycle by name (docs/0023 decision 2) because it has no temporal dead zone,
// and the analysis that separates this case from one whose cycle reads a
// `let` at evaluation time is a whole-graph reachability question over
// module-evaluation-time calls that is not built.
import { a } from './a.js';

console.log(a(5));

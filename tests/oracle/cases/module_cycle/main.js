// A module cycle whose crossing bindings are all hoisted `function`
// declarations, which ECMA-262 16.2.1.6.4 instantiates for the whole graph
// before any module body runs.
//
// a.js and b.js import each other. Neither is evaluated "first" in any useful
// sense, and neither needs to be: `a` and `b` are function declarations, so
// both exist before either body runs and the mutual recursion below is well
// defined. What makes the cycle safe to ACCEPT rather than merely possible is
// the temporal dead zone — a crossing binding that is not hoisted holds the
// uninitialized marker until its own declaration runs, so reading it too early
// is a ReferenceError and never a silent `undefined`. That half is pinned by
// cases/module_cycle_tdz.
import { a } from './a.js';

console.log(a(5));

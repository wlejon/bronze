// BLOCKED: the property-read forwarding and loop hoisting in
// src/lower/loop_prop_hoist.cpp assume the hooks the language calls implicitly
// on a boxed operand — ToPrimitive's `valueOf` / `toString`, under `+`, `*`,
// `<` and a template substitution — write no property
// (`isImpureForLoopPropHoist` in src/lower/loop_prop_analysis.cpp). Each hook
// below does, so the second read of `shared.v` answers the first read's value
// (1) instead of what the hook stored.
//
// Lifting it needs the pass to know an operand cannot be an object — a number
// or string proof per operand — rather than ending forwarding at every boxed
// arithmetic op, which would cost the whole dynamic path its hoisting.
//
// The getter, spread, rest and iterator forms of the same hazard are fixed and
// pinned in cases/prop_reread_across_user_code.js.

class Box { constructor() { this.v = 1; } }
const shared = new Box();
const reset = () => { shared.v = 1; };

const viaValueOf = (x) => { const a = shared.v; const r = x + 1; return [a, shared.v, r].join(" "); };
reset(); console.log("valueOf", viaValueOf({ valueOf() { shared.v = 9; return 1; } }));

const viaCompare = (x) => { const a = shared.v; const r = x < 2; return [a, shared.v, r].join(" "); };
reset(); console.log("compare", viaCompare({ valueOf() { shared.v = 8; return 1; } }));

const viaTemplate = (x) => { const a = shared.v; const r = `${x}`; return [a, shared.v, r].join(" "); };
reset(); console.log("template", viaTemplate({ toString() { shared.v = 2; return "s"; } }));

const loopValueOf = (x) => { let s = ""; for (let i = 0; i < 3; i++) { s += shared.v; const r = x * 2; } return s; };
reset(); console.log("loop valueOf", loopValueOf({ valueOf() { shared.v++; return 1; } }));

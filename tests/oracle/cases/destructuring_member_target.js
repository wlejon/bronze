// A destructuring assignment whose targets are property REFERENCES rather than
// names: `[o.a] = xs` and `({ k: o.b } = src)`.
//
// This is the half of destructuring that only the assignment form can spell — a
// binding form declares names, and `o.a` declares nothing — so nothing in
// cases/destructuring.js reaches it. What makes it worth its own case is not
// that the write happens but WHEN the reference for it is computed.
//
// From ECMA-262:
//
// 1. 13.15.5.5 AssignmentElement and 13.15.5.6 KeyedDestructuringAssignment
//    both begin the same way: if the DestructuringAssignmentTarget is neither
//    an ObjectLiteral nor an ArrayLiteral, its Evaluation — the REFERENCE, base
//    and key — happens FIRST, and only then is the element read out of the
//    source. So a getter on the source runs after the target's base expression,
//    not before it.
// 2. 13.15.2 step 3: the whole right-hand side is evaluated before the pattern
//    is walked at all, which is what the swap in cases/destructuring.js pins
//    and what makes `src` below print ahead of any target.
// 3. A default in a pattern fires on `undefined` and nothing else (8.6.2), and
//    that rule does not change when the target is a reference.
// 4. 13.15.5.2: a rest element in an array pattern collects the tail as a fresh
//    array, whatever the target it is written into.
const o = {};
[o.a, o.b] = [1, 2];
console.log(o.a, o.b);

({ p: o.c, q: o.d = 9 } = { p: 3 });
console.log(o.c, o.d);

// A computed index whose subexpression has a side effect: the targets are
// evaluated left to right, so the two calls happen in written order and the
// values land in slots 1 and 2.
const arr = [0, 0, 0, 0];
let k = 0;
function nextKey() { k = k + 1; return k; }
[arr[nextKey()], arr[nextKey()]] = [7, 8];
console.log(arr.join(","), k);

// Rule 1, made observable: the source's property is a getter, and the target's
// base is a call. "ref" must print before "get".
const trace = [];
const target = {};
const source = { get v() { trace.push("get"); return 11; } };
function base() { trace.push("ref"); return target; }
({ v: base().w } = source);
console.log(trace.join(","), target.w);

// Rule 2: the right-hand side runs before any target's base does.
const order = [];
function rhs() { order.push("rhs"); return { n: 4 }; }
function lhs() { order.push("lhs"); return o; }
({ n: lhs().e } = rhs());
console.log(order.join(","), o.e);

// Rule 4, and a nested pattern above a reference target: the array pattern's
// element is itself an object pattern, whose value is written through `o`.
[...o.tail] = [5, 6];
console.log(o.tail.join(","), o.tail.length);
[{ f: o.g }] = [{ f: 12 }];
console.log(o.g);

// A string key spelled as an index is the same write as a dotted one.
({ s: o["h"] } = { s: 13 });
console.log(o.h);

// Rule 3: `null` is not `undefined`, so the default does not fire for it.
({ t: o.i = 14 } = { t: null });
console.log(o.i);

// `return;` in a function that also returns a value somewhere else — the
// guard-clause shape (ECMA-262 14.10.1).
//
// A `ReturnStatement` with no expression returns UNDEFINED, which is a value:
// step 1 of 14.10.1 is "Return Completion Record { [[Type]]: return,
// [[Value]]: undefined }", not a completion with no value at all. bronze
// lowered it to a void return whatever the function's IL return type was, so
// a function that mixed the two forms emitted a `ret void` in a body typed to
// return a dynamic — rejected by the LLVM verifier, with the whole module
// refused. Falling off the END of the same function already produced the
// undefined; only the explicit spelling did not.
//
// The shape is not exotic: `if (x === undefined) return;` above the real work
// is how three.js writes an early exit, and it appears in Object3D,
// EventDispatcher, Material and BufferGeometry among others.
//
// What this pins:
//
// 1. A bare `return;` is `undefined` at the call site, and the same function's
//    other `return <expr>;` still returns its value.
// 2. It works from inside a loop, so the early exit may be a `break`-shaped
//    one, and from inside a method where the value return is the common path.
// 3. It runs the enclosing `finally` on the way out (14.15.3), and the value
//    it carries is still undefined afterwards.
// 4. `undefined` is what an assignment of the call's result sees, so
//    `typeof` of it is "undefined" rather than a hole.

function early(x) {
  if (x === undefined) return;
  return x * 2;
}
console.log(early(3), early(undefined));
console.log(typeof early(undefined), typeof early(3));

function firstIndex(list, target) {
  for (let i = 0; i < list.length; i++) {
    if (list[i] === target) return i;
  }
  return;
}
console.log(firstIndex([1, 2, 3], 2), firstIndex([1, 2, 3], 9));

const log = [];
function withFinally(bail) {
  try {
    if (bail) return;
    return "value";
  } finally {
    log.push(bail ? "bail" : "value");
  }
}
console.log(withFinally(true), withFinally(false), log.join(","));

class Box {
  constructor() {
    this.items = [];
  }
  add(v) {
    if (v === null) return;
    this.items.push(v);
    return this.items.length;
  }
}
const b = new Box();
console.log(b.add(null), b.add("a"), b.add("b"), b.items.join(","));

// An arrow with a block body takes the same path.
const pick = (v) => {
  if (v < 0) return;
  return v;
};
console.log(pick(-1), pick(4));

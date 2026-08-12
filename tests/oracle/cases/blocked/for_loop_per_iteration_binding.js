// BLOCKED: `unsupported construct: closure capturing the for-loop binding 'i'
// (per-iteration binding semantics); use a `let` declared inside the loop
// body`.
//
// ECMA-262 14.7.4.9 CreatePerIterationEnvironment: a `for` whose head is a
// LexicalDeclaration gets a fresh copy of every binding it declares before
// each iteration, with the previous iteration's value copied in. A closure
// created inside the body therefore captures THAT iteration's binding, and
// the three closures below see 0, 1 and 2 — not three views of one cell.
//
// bronze carries a loop variable as a block parameter across the back edge,
// which is one binding by construction, and its environment records are created
// on scope ENTRY — the loop header scope is entered once, above the header
// block, so a record made there is one record for the whole loop too. Making
// this work needs the environment created per iteration and threaded across the
// back edge, which is a change to the loop's shape and not to this diagnostic.
//
// It is named rather than miscompiled because the wrong answer is silent:
// every closure would return 3 and nothing would say so. The rule was later
// narrowed to say WHEN it fires — a closure with an `i` of its own no longer counts,
// pinned by `for_loop_binding_shadowing` — but a closure that really does
// reach the loop's binding must still be refused, which is what this pins.
//
// What this case pins when it lands:
//
// 1. An arrow created in the body captures the iteration's binding.
// 2. So does a `function` declared in the body, and a class method — every
//    closure kind reaches the same binding, so one mechanism must serve all.
// 3. A closure created in the HEAD itself (`for (let i = 0, f = () => i; …)`)
//    captures the loop environment the DECLARATION ran in, not any iteration's
//    copy — CreatePerIterationEnvironment makes the copies afterwards and the
//    update writes only to them, so that environment's `i` is 0 forever and
//    `f()` is 0 however many times the loop goes round. The answer is 0 and
//    not 3, which is why this shape is worth pinning separately from the
//    body's closures.
// 4. A closure over a binding the loop declares but never updates is the same
//    problem: `il` is copied per iteration exactly as `i` is.

const arrows = [];
for (let i = 0; i < 3; i++) {
  arrows.push(() => i);
}
console.log(arrows[0](), arrows[1](), arrows[2]());

const fns = [];
for (let i = 0; i < 3; i++) {
  function grab() {
    return i * 10;
  }
  fns.push(grab);
}
console.log(fns[0](), fns[1](), fns[2]());

const boxes = [];
for (let i = 0; i < 2; i++) {
  class Box {
    tag() {
      return "box" + i;
    }
  }
  boxes.push(new Box());
}
console.log(boxes[0].tag(), boxes[1].tag());

let first;
for (let i = 0, f = () => i; i < 3; i++) {
  if (i === 0) first = f;
}
console.log(first());

const both = [];
for (let i = 0, il = 3; i < il; i++) {
  both.push(() => i + "/" + il);
}
console.log(both[0](), both[2]());

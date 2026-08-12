// ECMA-262 14.7.4.9 CreatePerIterationEnvironment: a `for` whose head is a
// LexicalDeclaration gets a fresh copy of every binding it declares before each
// iteration, with the previous iteration's value copied in. A closure created
// inside the body therefore captures THAT iteration's binding, and the three
// arrows below see 0, 1 and 2 — not three views of one cell. The wrong answer
// here is silent: every closure would return 3 and nothing would say so.
//
// The loop's environment is not one record threaded round the back edge but a
// chain of siblings, which is what makes line 4 come out the way it does.
//
// What each block holds:
//
// 1. An arrow created in the body captures the iteration's binding.
// 2. So does a `function` declared in the body, and a class method — every
//    closure kind reaches the same binding, so one mechanism serves all three.
// 3. A closure created in the HEAD itself (`for (let i = 0, f = () => i; …)`)
//    captures the loop environment the DECLARATION ran in, not any iteration's
//    copy — CreatePerIterationEnvironment makes the copies afterwards and the
//    update writes only to them, so that environment's `i` is 0 forever and
//    `f()` is 0 however many times the loop goes round. The answer is 0 and
//    not 3, which is why this shape is pinned separately from the body's
//    closures: it is the one an implementation that reused a single record
//    would get wrong in the other direction.
// 4. A closure over a binding the loop declares but never updates is the same
//    rule: `il` is copied per iteration exactly as `i` is, and both are read
//    out of the copy the closure captured.
//
// Its sibling `for_loop_binding_shadowing` pins the other side — the loops
// whose closures share a SPELLING with the head binding and nothing else, and
// which must therefore keep the one-record shape.

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

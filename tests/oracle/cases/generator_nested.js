// Generators inside generators, generators that never suspend at all, and the
// boundary that keeps one body's `yield` out of another's.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.1.2: calling a generator function creates a generator object and runs
//   NONE of the body. So a generator whose whole body is a `console.log` prints
//   nothing until the first `next`.
// * 27.5.1.4 / 27.5.3.2: a body that finishes without ever suspending completes
//   on that first `next`, which answers `{ value: <return value>, done: true }`.
// * 15.5.1: `yield` binds to the generator whose body it is written in. A
//   function nested inside a generator has a body of its own, so a `yield` is
//   not even in scope there — which is why an inner generator's suspensions are
//   its own and the outer one has to drive it explicitly.
// * 13.2.4.1 spread drains an iterator, so an outer generator that re-yields an
//   inner one's values by hand produces one flat sequence.
// * Each call to a generator function makes a SEPARATE generator object with
//   its own execution context (27.5.1.2), so two walks of one function
//   interleave without sharing a position.
//
// NOT IN THIS CASE, and refused by name rather than answered wrongly:
// `yield* inner()`, the syntax that would replace the hand-written driver
// below. Delegation (27.5.3.7) is a second walk suspended inside the outer one,
// including forwarding `next`, `return` and `throw` through to the inner
// iterator; `src/parse/parser_generator.cpp` reports
// "unsupported construct: `yield*` (delegation to another iterable)".

// --- one generator driving another by hand --------------------------------
function* inner() {
    yield 'a';
    yield 'b';
}
function* outer() {
    yield 1;
    const walk = inner();
    let step = walk.next();
    while (!step.done) {
        yield step.value;
        step = walk.next();
    }
    yield 2;
}
console.log([...outer()]);

// The inner walk is a binding of the outer generator's frame, so it survives
// every suspension: drive the outer one by hand and the inner one does not
// restart.
const o = outer();
console.log(o.next().value, o.next().value, o.next().value, o.next().value);
console.log(o.next());

// --- a generator nested INSIDE a generator body ---------------------------
// `make` is declared in the outer body and yielded out of it; its own `yield`
// belongs to it, not to the outer one.
function* factory() {
    function* made(tag) {
        yield tag + '1';
        yield tag + '2';
    }
    yield [...made('x')];
    yield [...made('y')];
}
console.log([...factory()]);

// --- a generator whose body never suspends --------------------------------
function* silent() {
    console.log('body');
}
const s = silent();
console.log('created');
console.log(s.next());
console.log(s.next());
console.log([...silent()]);

// --- a generator that only returns ----------------------------------------
function* onlyReturns() {
    return 7;
}
console.log(onlyReturns().next());
console.log([...onlyReturns()]);

// --- an ordinary function inside a generator ------------------------------
// The nested function is not a generator and has no suspension of its own; it
// simply reads the frame it closes over.
function* closes() {
    let n = 0;
    const bump = () => {
        n = n + 1;
        return n;
    };
    yield bump();
    yield bump();
    yield n;
}
console.log([...closes()]);

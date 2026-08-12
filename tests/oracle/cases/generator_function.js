// `function* g() {}` — the generator DECLARATION and expression forms, and
// the timing that makes a generator a generator rather than a function that
// returns an array (docs/0026).
//
// From ECMA-262 27.5.3.2 (GeneratorResume), 15.5.3 (a generator function's
// body does not run when it is called; calling it creates the generator
// object) and 15.5.1 (`yield` with no operand yields undefined):
//
// 1. Calling a generator function runs NONE of the body. The statements
//    before the first `yield` run on the first `next()`; the statements
//    between two yields run on the call that produces the second; the
//    statements after the last one run on the call that reports `done`.
// 2. Those trailing statements run exactly ONCE. The call after `done` runs
//    nothing at all.
// 3. Each call to the generator function is its own walk with its own step,
//    so two live iterators from one function interleave without interfering.
// 4. Parameters are visible to every step: the body reads them where they
//    were bound, not where the iterator is consumed.
// 5. A generator with no `yield` at all is immediately done, and `yield;`
//    with no operand yields `undefined`.

function* pair(a, b) {
    yield a;
    yield b;
}

console.log([...pair(1, 2)]);
for (const value of pair('x', 'y')) console.log(value);

// 1 and 2 — when each stretch of the body runs.
function* traced() {
    console.log('before the first yield');
    yield 1;
    console.log('between the yields');
    yield 2;
    console.log('after the last yield');
}

const t = traced();
console.log('created, nothing has run');
console.log(t.next());
console.log(t.next());
console.log(t.next());
console.log(t.next());

// 3 — two walks of one generator function, interleaved.
const p = pair(1, 2);
const q = pair(3, 4);
console.log(p.next().value, q.next().value, p.next().value, q.next().value);

// 4 — a function EXPRESSION generator, closing over its parameter.
const times = function* (n) {
    yield n * 1;
    yield n * 2;
    yield n * 3;
};
console.log([...times(5)]);

// 5 — the two degenerate bodies.
const empty = function* () {};
console.log([...empty()]);
console.log(empty().next());

function* bare() {
    yield;
    yield 5;
}
console.log([...bare()]);

// A generator function is an ordinary binding: it can be passed and stored.
const chosen = pair;
console.log([...chosen('a', 'b')]);

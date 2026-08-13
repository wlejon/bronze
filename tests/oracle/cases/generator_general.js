// Generators, end to end: the five things a program does with one, in one
// file, so that they are pinned TOGETHER rather than only apart. Each of the
// five also has a case of its own — cases/generator_loops.js,
// generator_delegation.js, generator_resume_values.js,
// generator_return_statement.js and generator_abrupt_resume.js — and this one
// exists because a state machine can be right about each of them in isolation
// and wrong about a body that holds several.
//
// The expectation was derived BY HAND from ECMA-262: 27.5.3.2
// (GeneratorResume: the argument of `next` becomes the value of the `yield`
// that suspended it), 27.5.3.3 (GeneratorResumeAbrupt, which is what
// `return` on a generator object does), 27.5.3.7 (`yield*` delegates to
// another iterable and yields everything it yields) and 13.2.4.1 (spread
// collects the yielded values and discards the completion value):

// 1 — a `yield` inside a loop, resumed rather than restarted.
function* count(n) {
    for (let i = 0; i < n; i++) {
        yield i;
    }
}
console.log([...count(4)]);

// 2 — `yield*` delegation.
function* inner() {
    yield 'a';
    yield 'b';
}

function* outer() {
    yield 1;
    yield* inner();
    yield 2;
}
console.log([...outer()]);

// 3 — the value of a `yield` is what `next(v)` supplied, and a binding
// carries it across the suspension.
function* echo() {
    const first = yield 'ask';
    const second = yield first + '!';
    return second;
}
const e = echo();
console.log(e.next());
console.log(e.next('hi'));
console.log(e.next('bye'));

// 4 — `return <expr>` ends the walk and becomes the final `value`. Spread
// discards it; a hand-driven `next` sees it.
function* early(stop) {
    yield 1;
    if (stop) return 'stopped';
    yield 2;
}
console.log([...early(true)]);
console.log([...early(false)]);
const stopped = early(true);
stopped.next();
console.log(stopped.next());

// 5 — the generator object's own `return` method closes it early.
const c = count(10);
console.log(c.next().value);
console.log(c.return(99));
console.log(c.next());

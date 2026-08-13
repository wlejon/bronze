// BLOCKED: ``unsupported construct: `yield*` (delegation to another
// iterable)``, from case 2 below and from nothing else.
//
// Everything else this case asks for is BUILT. Generators are a real state
// machine now: the body becomes a resume function entered at its top on the
// first `next` and just after a `yield` on every later one, every binding that
// could cross a suspension lives in the frame's environment record, and the
// generator object carries `[[GeneratorState]]` with `next`/`return`/`throw` on
// %GeneratorPrototype% (27.5.3). Cases 1, 3, 4 and 5 are covered on the
// unblocked side by cases/generator_loops.js, generator_resume_values.js,
// generator_return_statement.js and generator_abrupt_resume.js. This file stays
// here, unchanged, because it also asks for delegation, and one `yield*` is
// enough to keep the whole file failing to compile — which is exactly what a
// blocked case is for.
//
// The remaining blocker is delegation, and it is a protocol rather than one
// more entry in a state table. 27.5.3.7 makes `yield* x` a loop that
// GetIterator's `x` and forwards every resumption to it: `next(v)` becomes
// `innerNext.call(iterator, v)`, `throw(e)` looks for the inner iterator's
// `throw` and calls IteratorClose with a TypeError if it has none, and
// `return(v)` looks for its `return` and, if the inner result is not done,
// keeps yielding. So there are two live positions, and the outer one's resume
// point has to re-enter a loop whose state is an iterator object plus a
// received completion — three more suspension shapes at one syntactic site.
// That is a chunk of its own, not a line in `lowerYield`.
//
// What this case pins when it lands, from ECMA-262 27.5.3.2
// (GeneratorResume: the argument of `next` becomes the value of the `yield`
// that suspended it), 27.5.3.3 (GeneratorResumeAbrupt, which is what
// `return` on a generator object does), 15.5.5 (`yield*` delegates to
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

// BLOCKED: `unsupported construct: a `yield` inside a loop; bronze implements
// the straight-line subset only...`.
//
// Generators shipped as a DESUGARING rather than as a coroutine: a body that is
// a straight line of `yield <expr>;` statements becomes an iterator object
// whose `next` switches on a step index. This case is the receipt for that
// trade — everything a real generator can do that an index switch cannot.
//
// The blocker is the shape of the transform, not the syntax. An index switch
// re-enters the body FROM THE TOP on every `next`, so:
//
//  - a `yield` inside a loop would restart the loop rather than resume it,
//    and the loop's own counter would not survive the return in between;
//  - a binding declared between two yields cannot live in `next`'s frame,
//    because that frame is gone by the time the next call arrives;
//  - the VALUE of a `yield` is what `next(v)` was called with, which arrives
//    on a call that has not happened yet, so there is nothing to substitute;
//  - `yield*` is a nested walk suspended inside the outer one, which is two
//    live positions where the index carries one;
//  - `return <expr>` terminates the walk from wherever it is written and
//    supplies the final result's `value`, and `done: true` in bronze carries
//    no value at all.
//
// A general implementation is a state-machine transform: every binding that
// crosses a yield is lifted into a state object, the body is cut into basic
// blocks at each yield, and `next` becomes a dispatch over the block index
// with the resumption value delivered as its parameter. That is an IL-level
// transform, not a parser one, and it is why this is a chunk of its own
// rather than three more cases in generator_function.js.
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

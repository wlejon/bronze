// `return` inside a generator body: it ends the walk, and its operand becomes
// the `value` of the result that reports `done: true`.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.3.2 / 27.5.1.4 GeneratorStart: when the body finishes with a normal or
//   return completion, the generator's state becomes `completed` and the result
//   is CreateIterResultObject(the completion's value, true).
// * 14.10.1: `return;` completes with value `undefined`, `return expr;` with
//   the value of `expr`. So the final result's `value` is `undefined` for the
//   bare form.
// * Falling off the end of the body is a normal completion with value
//   `undefined` (10.2.1.4 / 27.5.1.4 step 4), which is why the last result of
//   an ordinary drained generator says `{ value: undefined, done: true }`.
// * 7.4.6 IteratorClose is not involved here: the walk ended by itself.
// * 13.2.4.1 array spread and 25.1.4.2: spread stops at the first `done: true`
//   result and DOES NOT include its `value`. So the completion value of a
//   generator is invisible to `[...g()]` — which is the one asymmetry between
//   spreading a generator and driving it by hand.
// * 27.5.3.2 step 3: once completed, every later `next` answers
//   `{ value: undefined, done: true }`. A generator does not restart.

// --- `return <expr>;` reached from inside a branch ------------------------
function* early(stop) {
    yield 1;
    if (stop) return 'stopped';
    yield 2;
}
// Spread never sees 'stopped': it is the completion value, not a yielded one.
console.log([...early(true)]);
console.log([...early(false)]);
// Driven by hand, it is the `value` of the result that says done.
const s = early(true);
console.log(s.next());
console.log(s.next());
console.log(s.next());

// --- `return;` with no operand -------------------------------------------
function* bare() {
    yield 'a';
    return;
    // Nothing below here runs.
}
const b = bare();
console.log(b.next());
console.log(b.next());
console.log(b.next());

// --- a `return` with no `yield` before it ---------------------------------
// The body runs on the FIRST `next`, not on the call, so the completion
// arrives from that call rather than from the generator function itself.
function* immediate() {
    return 'first';
}
console.log(immediate().next());
console.log([...immediate()]);

// --- a `return` out of the middle of a loop -------------------------------
function* upTo(limit) {
    for (let i = 0; i < 10; i++) {
        if (i === limit) return 'hit ' + i;
        yield i;
    }
    return 'never';
}
console.log([...upTo(3)]);
const u = upTo(2);
console.log(u.next().value, u.next().value, u.next());

// --- a generator that only falls off the end ------------------------------
function* falls() {
    yield 'x';
}
const f = falls();
console.log(f.next());
console.log(f.next());

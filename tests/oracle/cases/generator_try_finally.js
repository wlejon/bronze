// A `try`/`finally` around a `yield`: the cleanup runs on every way out of the
// protected region, including the two that arrive from OUTSIDE the generator.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 14.15.3 TryStatement: the Finally block is evaluated for every completion
//   of the Block — normal, return, throw, break, continue — and if it completes
//   normally the original completion is what the statement yields.
// * A `yield` is not a completion of the block: 27.5.1.1 GeneratorYield
//   suspends the execution context and RESUMES it later at the same point, so
//   passing over a `yield` inside a `try` does not run the `finally`. The
//   cleanup runs when control finally leaves the block.
// * 27.5.3.3 `Generator.prototype.return` resumes with a RETURN completion at
//   the suspension point. That completion leaves the `try`, so the `finally`
//   runs — which is exactly why `for-of` with a `break` is safe over a
//   generator that holds a resource.
// * 27.5.3.4 `Generator.prototype.throw` resumes with a THROW completion at the
//   suspension point; the `finally` runs on its way out and, with no `catch`,
//   the exception continues to the caller of `throw()`.
// * 14.10.1 `return expr;` inside the `try` is a return completion, so the
//   `finally` runs before the generator reports `{ value: expr, done: true }`.
//
// NOT IN THIS CASE, and refused by name rather than answered wrongly: a `yield`
// written INSIDE a `finally` block. bronze lowers a `finally` body once per way
// out of the protected region, so a suspension in one would need a resume point
// per copy; `src/ast/yield_lift.cpp` reports
// "unsupported construct: a `yield` inside a `finally` block".

// --- passing over a yield does not run the finally ------------------------
// 'finally ran' appears ONCE, when the block is left after the second yield.
function* cleanup() {
    try {
        yield 'a';
        yield 'b';
    } finally {
        console.log('finally ran');
    }
    yield 'c';
}
console.log([...cleanup()]);

// --- gen.return() out of the middle of the try ----------------------------
const g = cleanup();
console.log(g.next());
console.log(g.return('stop'));
console.log(g.next());

// --- a `return` written inside the try ------------------------------------
function* returning() {
    try {
        yield 1;
        return 'from try';
    } finally {
        console.log('finally on return');
    }
}
const r = returning();
console.log(r.next());
console.log(r.next());

// --- gen.throw() out of the middle of the try -----------------------------
function* rethrows() {
    try {
        yield 1;
    } finally {
        console.log('finally on throw');
    }
}
const t = rethrows();
console.log(t.next());
try {
    t.throw('bang');
} catch (err) {
    console.log('out:', err);
}
console.log(t.next());

// --- catch and finally together, with the yield in the catch --------------
function* both() {
    try {
        yield 'try';
    } catch (err) {
        console.log('caught', err);
        yield 'catch';
    } finally {
        console.log('finally');
    }
    yield 'end';
}
const b = both();
console.log(b.next());
console.log(b.throw('e1'));
console.log(b.next());
console.log(b.next());

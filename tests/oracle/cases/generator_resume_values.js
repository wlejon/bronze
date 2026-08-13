// The VALUE of a `yield`: what `next(v)` supplied, arriving at the suspension
// point, and the bindings that carry values across a suspension.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.3.2 GeneratorResume passes its `value` argument to
//   GeneratorResumeAbstractClosure, and 27.5.1.1 GeneratorYield returns that
//   value as the result of the Yield expression. So `yield x` EVALUATES to the
//   argument of the `next` that resumed it — not to `x`, which is what the
//   suspension handed OUT.
// * The first `next()` runs the body from its top to the first `yield`, so
//   whatever argument that first call carries has no Yield expression waiting
//   for it and is discarded (27.5.3.2 step 6 resumes a suspendedStart generator
//   at the start of the body).
// * A `next()` with no argument supplies `undefined` (10.4.1 / the default for
//   an absent argument), so `(yield 5) || 'fallback'` takes the right operand
//   (13.13.1: `||` returns the left operand only if ToBoolean of it is true).
// * 15.5.1: `yield` is an AssignmentExpression, legal wherever one is — an
//   initializer, an argument, an operand of `+`, the condition of a
//   conditional. Its value participates like any other.
// * 13.15.2 and 13.4.4: evaluation is left to right, so in `(yield 1) + (yield 2)`
//   the first suspension happens before the second and both values are in hand
//   before the addition.

// --- what `next(v)` supplies is what the `yield` evaluates to -------------
function* echo() {
    const first = yield 'ask';
    const second = yield first + '!';
    return second;
}
const e = echo();
console.log(e.next());
console.log(e.next('hi'));
console.log(e.next('bye'));
console.log(e.next('ignored'));

// --- a binding declared BETWEEN two yields survives the call in between ---
function* between() {
    yield 'one';
    const mid = 'held';
    yield 'two';
    yield mid;
}
console.log([...between()]);

// --- the resumption value threaded through a loop's accumulator -----------
// Each `next(v)` adds `v` to a total that lives across every suspension.
function* adder() {
    let total = 0;
    for (let i = 0; i < 3; i++) {
        const got = yield total;
        total = total + got;
    }
    return total;
}
const a = adder();
console.log(a.next().value, a.next(1).value, a.next(10).value, a.next(100).value);

// --- yield in every expression position it is legal in --------------------
function label(v) {
    return '<' + v + '>';
}
function* positions() {
    const sum = (yield 1) + (yield 2);
    console.log('sum', sum);
    console.log('arg', label(yield 3));
    const flag = (yield 4) ? 'yes' : 'no';
    yield flag;
    const chosen = (yield 5) || 'fallback';
    yield chosen;
}
const p = positions();
console.log(p.next());
console.log(p.next(10));
console.log(p.next(20));
console.log(p.next('x'));
console.log(p.next(true));
console.log(p.next(0));
console.log(p.next());
console.log(p.next());

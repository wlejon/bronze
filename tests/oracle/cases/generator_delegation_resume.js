// `yield*` — what a delegation does with a resumption it did not ask for. A
// `next(v)` carries a value INTO the inner iterator, a `throw(e)` is handed to
// the inner iterator's own `throw`, and an inner iterator with no `throw` at
// all is the one clause of 27.5.3.7 that is easy to get half right.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.3.7 step 4: the delegation starts with a NORMAL completion carrying
//   `undefined`, so the first thing it does is call the inner `next` with no
//   useful argument — whatever the outer `next` was called with is discarded,
//   exactly as 27.5.3.2 discards the argument of the first `next` to a
//   generator that has not started.
// * 27.5.3.7 step 5.a.i: a normal resumption calls the inner `next` with
//   `received.[[Value]]`, so `outer.next(v)` becomes `inner.next(v)` and `v`
//   is the value of the INNER generator's suspended `yield`.
// * 27.5.3.7 step 5.b.i and 5.b.ii: a throw resumption reads the inner
//   iterator's `throw` (7.3.11 GetMethod) and calls it with the exception. For
//   an inner generator that is 27.5.3.4, which raises the exception at the
//   inner generator's own suspension point — so a `try` written inside the
//   inner body catches it, and the delegation continues with whatever it
//   yields next.
// * 27.5.3.7 step 5.b.iii: an inner iterator with NO `throw` method is
//   IteratorClosed FIRST — 7.4.9 calls its `return` with no arguments — and a
//   TypeError is raised only afterwards, to report the protocol violation.
//   Close then throw, not throw instead of close.
// * 7.4.9 step 4: an iterator with no `return` closes by doing nothing, so the
//   TypeError arrives on its own for an array (23.1.5.2 gives
//   %ArrayIteratorPrototype% a `next` and nothing else).
// * 27.5.1.4: an abrupt completion of a generator body leaves the generator
//   completed, so a `throw` that escapes it is not a suspension it can be
//   resumed from.

// --- A: `next(v)` forwarded through the delegation ------------------------
function* adder() {
    const first = yield 'give me one';
    const second = yield first + 1;
    return first + second;
}

function* driver() {
    const sum = yield* adder();
    yield sum * 2;
}

const d = driver();
console.log(d.next());
console.log(d.next(10));
console.log(d.next(5));
console.log(d.next());
console.log(d.next());

// --- B: `throw(e)` forwarded to an inner generator that catches it --------
function* resilient() {
    try {
        yield 'a';
    } catch (err) {
        console.log('inner caught', err);
        yield 'recovered';
    }
    yield 'b';
    return 'inner done';
}

function* outerThrow() {
    const end = yield* resilient();
    yield end;
}

const t = outerThrow();
console.log(t.next());
console.log(t.throw('kaboom'));
console.log(t.next());
console.log(t.next());
console.log(t.next());

// --- C: `throw(e)` to an iterator with no `throw`, but with a `return` ----
// The `return` really runs, and the TypeError arrives after it.
const noThrowIterator = {
    [Symbol.iterator]() {
        return this;
    },
    next() {
        return { value: 'only', done: false };
    },
    return(v) {
        console.log('inner return ran, with', v);
        return { value: 'closed', done: true };
    }
};

function* delegatesToIt() {
    try {
        yield* noThrowIterator;
    } catch (err) {
        console.log('outer caught a TypeError:', err instanceof TypeError);
        yield 'after the violation';
    }
}

const v = delegatesToIt();
console.log(v.next());
console.log(v.throw('never reaches the inner iterator'));
console.log(v.next());

// --- D: the same violation over an array, which has nothing to close ------
function* delegatesToArray() {
    yield* [1, 2, 3];
}

const w = delegatesToArray();
console.log(w.next());
try {
    w.throw('nowhere to go');
} catch (err) {
    console.log('TypeError:', err instanceof TypeError);
}
console.log(w.next());

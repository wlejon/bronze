// `yield*` — what a delegation does with a RETURN resumption, which is the half
// of 27.5.3.7 that has three different answers depending on the inner iterator.
// `gen.return(v)` on a delegating generator does not simply end it: whether it
// ends, what value it ends with, and whether it ends at all are the inner
// iterator's to decide.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.3.3 `Generator.prototype.return` resumes the generator with a RETURN
//   completion, which reaches the delegation as `received.[[Type]]` of return.
// * 27.5.3.7 step 5.c.ii and 5.c.iii: the inner iterator's `return` is read
//   with GetMethod, and an iterator that HAS NONE lets the completion pass
//   straight through — the outer generator returns the value the caller gave
//   it, and the delegation contributes nothing.
// * 27.5.3.7 step 5.c.iv and 5.c.viii: an iterator that has one is CALLED with
//   that value, and if its result reports done the outer walk ends carrying
//   the INNER result's `value` — not the value `return(v)` was called with.
// * 27.5.3.7 step 5.c.x: if the inner result does NOT report done, the
//   delegation keeps going. It yields that result onward and waits to be
//   resumed again, so an inner iterator can refuse to be closed.
// * 27.5.1.3 puts `return` on %GeneratorPrototype%, so an inner GENERATOR
//   always has one: the return completion reaches its suspension point,
//   completes it, and comes back done.
// * 23.1.5.2 gives %ArrayIteratorPrototype% only `next`, so an array is the
//   iterator with no `return` at all.
// * In every case the code after the `yield*` in the outer body does not run:
//   a return completion leaves the delegation, it does not fall out of it.

// --- A: an inner iterator WITH a `return`, whose result is done -----------
const closable = {
    [Symbol.iterator]() {
        return this;
    },
    next() {
        return { value: 'tick', done: false };
    },
    return(v) {
        console.log('closable.return got', v);
        return { value: 'closed with ' + v, done: true };
    }
};

function* usesClosable() {
    yield* closable;
    console.log('never reached');
}

const a = usesClosable();
console.log(a.next());
console.log(a.return(7));
console.log(a.next());

// --- B: an inner iterator with NO `return`: the completion passes through --
function* overArray() {
    yield* [1, 2, 3];
    console.log('never reached either');
}

const b = overArray();
console.log(b.next());
console.log(b.return(42));
console.log(b.next());

// --- C: an inner `return` that reports NOT done, and so refuses to close ---
const stubborn = {
    [Symbol.iterator]() {
        return this;
    },
    next(v) {
        return { value: 'next:' + v, done: false };
    },
    return(v) {
        return { value: 'refused:' + v, done: false };
    }
};

function* neverCloses() {
    yield* stubborn;
}

const c = neverCloses();
// The argument of the FIRST `next` is discarded — the body has not started, so
// there is no suspended `yield` for it to be the value of — and the delegation
// then opens with `undefined` of its own (step 4).
console.log(c.next(1));
console.log(c.return(2));
console.log(c.next(3));

// --- D: an inner GENERATOR, which always has a `return` -------------------
function* innerGen() {
    yield 'p';
    yield 'q';
}

function* outerGen() {
    yield* innerGen();
    yield 'unreached';
}

const e = outerGen();
console.log(e.next());
console.log(e.return('stop'));
console.log(e.next());

// --- E: numbers through every seam of the delegation ----------------------
// The `yield*` expression's value used in arithmetic, and a `return(v)` whose
// number travels back out through the inner generator's `return`. A string
// survives a lost coercion where a number does not, so these are the lines a
// divergence between the inference and `--no-infer` builds would show up in.
function* numbers() {
    yield 1;
    yield 2;
    return 3;
}

function* arithmetic() {
    const got = yield* numbers();
    yield got / 2;
    yield got - 0.5;
}
console.log([...arithmetic()]);

const f = arithmetic();
console.log(f.next());
console.log(f.return(-1.5));

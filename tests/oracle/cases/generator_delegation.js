// `yield*` — delegation, when the walk runs to its end. What a `yield*` YIELDS,
// what its expression is worth, and what counts as an iterable to delegate to.
// The three abrupt resumptions are cases/generator_delegation_resume.js and
// cases/generator_delegation_return.js.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.3.7 step 2: `yield* x` performs GetIterator on `x` ONCE, so anything
//   iterable can be delegated to — an array and a string as readily as another
//   generator. Nothing in the algorithm mentions generators at all.
// * 27.5.3.7 step 5.a: a normal resumption calls the inner iterator's `next`;
//   step 5.a.vi yields the inner result ONWARD, so everything the inner
//   iterator produces appears in the outer walk, in order, unchanged.
// * 27.5.3.7 step 5.a.v: when an inner result reports done, the delegation is
//   over and IteratorValue of THAT result — the inner generator's `return`
//   value — is the value of the whole `yield*` expression. What the inner
//   generator yielded is not it, and a `return` value is not yielded.
// * 13.2.4.1 / 23.1.3.34: spread drives `next` until done and collects the
//   yielded values, discarding the completion value — which is why the
//   `yield*` expression's worth is printed by the body rather than shown in
//   the spread.
// * 23.1.5.2.1: an exhausted array iterator answers `{ value: undefined,
//   done: true }`, so delegating to an empty array yields nothing and the
//   expression is `undefined`.
// * 22.1.3.36: a string's iterator steps by code point.
// * 24.2.3.11 and 24.1.3.13: a Set's default iterator yields its values and a
//   Map's yields `[key, value]` pairs, which is what a `for-of` over either
//   already sees.

// 1 — delegation to another generator: everything it yields, in order, then
// back to the outer body.
function* letters() {
    yield 'a';
    yield 'b';
}

function* withLetters() {
    yield 1;
    yield* letters();
    yield 2;
}
console.log([...withLetters()]);

// 2 — delegation to a plain array. An ARRAY is not a generator and has no
// `throw` and no `return`; it is iterable, and that is the whole requirement.
function* fromArray() {
    yield* [10, 20, 30];
    yield 40;
}
console.log([...fromArray()]);

// 3 — the value of the `yield*` EXPRESSION is the inner generator's return
// value. Numeric on purpose: a build that lost the boxing on the way out of the
// delegation would still print the right STRING, and would not print the right
// number.
function* counted() {
    yield 1;
    yield 2;
    return 7;
}

function* usesTotal() {
    const total = yield* counted();
    yield total + 100;
    yield total * 2;
}
console.log([...usesTotal()]);

// 4 — nested delegation: a `yield*` inside a generator that is itself being
// delegated to. Each level's expression is worth the level below's `return`,
// and a yield from the bottom travels up through both.
function* leaf() {
    yield 'x';
    return 3;
}

function* mid() {
    const a = yield* leaf();
    yield a + 1;
    return a * 10;
}

function* top() {
    const b = yield* mid();
    yield b + 5;
}
console.log([...top()]);

// 5 — an empty iterable. The first `next` already reports done, so the
// delegation yields nothing at all and its value is that result's `value`.
function* emptyDelegate() {
    const v = yield* [];
    console.log('empty gave', v);
    yield 'after';
}
console.log([...emptyDelegate()]);

// 6 — a string, stepped by code point.
function* spellOut() {
    yield* 'hi';
}
console.log([...spellOut()]);

// 7 — a `yield*` inside a LOOP. One syntactic site, a fresh iterator every time
// round: the delegation is entered, driven to done and left before the loop's
// next iteration reaches it again.
function* pairsUpTo(n) {
    let i = 0;
    while (i < n) {
        yield* [i, i + 1];
        i = i + 2;
    }
}
console.log([...pairsUpTo(4)]);

// 8 — the other iterables the runtime knows.
function* fromCollections() {
    yield* new Set([1, 2]);
    yield* new Map([['k', 9]]);
}
console.log([...fromCollections()]);

// 9 — two delegations to the SAME generator function. Each call makes its own
// generator object, so the second walk starts from the top rather than
// continuing the first.
function* twice() {
    yield* letters();
    yield* letters();
}
console.log([...twice()]);

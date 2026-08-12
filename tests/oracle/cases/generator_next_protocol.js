// The iterator a generator returns, driven BY HAND rather than by a loop.
// `for-of` hides every observable of the protocol behind one syntax; this case
// calls `next()` itself, so the shape of each result — and what happens on the
// call after the last one — is pinned rather than implied.
//
// From ECMA-262 7.4.1 (IteratorResult: an ordinary object with `value` and
// `done`), 27.5.1.2 (`Generator.prototype[@@iterator]` returns the generator
// itself), 27.5.3.2 (`next` on a completed generator returns
// `{ value: undefined, done: true }`) and 7.4.2 (GetIterator calls the
// method with the object as the receiver):
//
// 1. Each `next()` before the end answers `{ value: <yielded>, done: false }`
//    — in that property order, because that is the order 7.4.1 builds it in
//    and own keys print in creation order.
// 2. The call that runs off the end answers `{ value: undefined, done: true }`,
//    and so does every call after it: a completed iterator stays completed
//    rather than restarting.
// 3. A generator object IS its own iterable: `it[Symbol.iterator]()` is `it`,
//    so a half-drained iterator can be handed to `for-of` or to spread and
//    continues from where it was.
// 4. `[Symbol.iterator]` read off the object and called through `o[k]()` runs
//    with `o` as its receiver (13.3.6.1 evaluates the MemberExpression once
//    and passes its base as the this value), which is what makes `this.base`
//    inside the body mean this instance.
//
// The generator object's `[Symbol.iterator]` is INHERITED, exactly as 27.5.1.2
// defines it: bronze builds the object against a %GeneratorPrototype% carrying
// the self-hook, so its own keys are `next` and nothing else — which is why
// `Object.getOwnPropertySymbols` of one is empty in cases/
// collection_internal_slots.js. DELIBERATE DIVERGENCE: that prototype carries
// no `return` and no `throw` method, so `typeof` its `next` is the only thing
// about its identity this case may pin.

class Pair {
    constructor(base) {
        this.base = base;
    }

    *[ Symbol.iterator ]() {
        yield this.base;
        yield this.base + 1;
    }
}

const r = new Pair(10);

// 1 and 2 — every result object, including the two past the end.
const it = r[Symbol.iterator]();
console.log(it.next());
console.log(it.next());
console.log(it.next());
console.log(it.next());

// The result is an ordinary object: its own keys, in creation order.
const result = r[Symbol.iterator]().next();
console.log(Object.keys(result));
console.log(result.value, result.done);
console.log(typeof it.next);

// 3 — the iterator is its own iterable, and a partial drain shows through.
const it2 = r[Symbol.iterator]();
console.log(it2[Symbol.iterator]() === it2);
console.log(it2.next().value);
console.log([...it2]);
console.log([...it2]);

// 4 — a fresh call gives a fresh walk, on the right receiver.
console.log([...r[Symbol.iterator]()]);
const other = new Pair(100);
console.log([...other[Symbol.iterator]()]);

// The step index belongs to the CALL, not to the object: two iterators taken
// from one instance advance independently.
const p = r[Symbol.iterator]();
const q = r[Symbol.iterator]();
console.log(p.next().value, q.next().value, p.next().value, q.next().value);
console.log(p.next().done, q.next().done);

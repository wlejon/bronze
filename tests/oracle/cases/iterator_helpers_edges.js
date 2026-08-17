// The iterator helpers' EDGES (ECMA-262 27.1.4): what each refuses, what each
// closes, and what an exhausted or abandoned helper does next.
//
// Derived from ECMA-262:
//
// 1. Every helper's step 2 is "if O is not an Object, throw a TypeError", so
//    `Iterator.prototype.map.call(1, f)` throws and never reads `next`.
// 2. A non-callable callback is `IteratorClose(O, error)` (each member's step
//    3.b): the TypeError is raised AND the receiver is closed. The close is
//    observable through a generator's `finally`, which is why every case below
//    uses a generator that has already been STARTED — 27.5.3.3 on a generator
//    still at suspendedStart completes it without entering the body, so a
//    `finally` in an unstarted generator legitimately never runs.
// 3. `take` / `drop` (27.1.4.1.11, 27.1.4.1.4): ToNumber first, then NaN is a
//    RangeError and a negative count is a RangeError — both closing the
//    receiver. ToIntegerOrInfinity turns -0.5 into +0, so THAT is not negative
//    and not an error. A `valueOf` that throws closes the receiver too (step
//    4's IfAbruptCloseIterator) and its own error is what propagates.
// 4. 27.1.4.1.11 step 6.b: `take`'s count running out closes the underlying
//    iterator, so the generator is left finished rather than suspended.
// 5. A callback that throws closes the underlying iterator and the CALLBACK's
//    error propagates (7.4.12 IfAbruptCloseIterator discards any error the
//    `return` raises).
// 6. 27.1.4.2.2 `return` on a helper closes the underlying iterator and marks
//    the helper done; a `break` out of a for-of over a helper reaches it. Once
//    done — by exhaustion, by `return`, or by a throw — a helper answers
//    `{ value: undefined, done: true }` for ever and never touches the
//    underlying iterator again.
// 7. `flatMap` opens each mapped value with GetIteratorFlattenable under
//    reject-primitives (27.1.4.1.7 step 6.d.i), so a STRING the mapper returns
//    is a TypeError rather than an iteration of its characters.
// 8. 27.1.3.1.1 `Iterator.from` accepts an object or a String primitive and
//    refuses every other primitive; a value whose @@iterator is not callable is
//    a TypeError too.
// 9. `Iterator` itself is abstract (27.1.3.1): calling it and `new Iterator()`
//    are both a TypeError, while a subclass's `super()` succeeds and its
//    instances inherit all eleven helpers.
function* logged(tag) {
  try {
    yield 1;
    yield 2;
    yield 3;
    yield 4;
    yield 5;
  } finally {
    console.log(`closed ${tag}`);
  }
}
// A generator that has entered its `try`, so a close is observable.
function started(tag) {
  const g = logged(tag);
  g.next();
  return g;
}
function* count(n) {
  for (let i = 1; i <= n; i += 1) yield i;
}
function reason(fn) {
  try {
    fn();
    return 'no throw';
  } catch (e) {
    return e.name;
  }
}

// 1: a non-object receiver.
console.log(reason(() => Iterator.prototype.map.call(1, (x) => x)));
console.log(reason(() => Iterator.prototype.toArray.call(undefined)));
console.log(reason(() => Iterator.prototype.take.call(null, 1)));

// 2: a non-callable callback closes the receiver first.
console.log(reason(() => started('map-arg').map(undefined)));
console.log(reason(() => started('filter-arg').filter(7)));
console.log(reason(() => started('reduce-arg').reduce('nope')));
console.log(reason(() => started('some-arg').some({})));

// 3: take / drop argument validation.
console.log(reason(() => started('take-nan').take(NaN)));
console.log(reason(() => started('take-neg').take(-1)));
console.log(reason(() => started('drop-nan').drop('abc')));
console.log(reason(() => started('take-valueof').take({ valueOf() { throw new SyntaxError('v'); } })));
// Neither of these is an error: ToNumber('2') is 2 and ToIntegerOrInfinity(-0.5) is +0.
console.log(count(5).take('2').toArray().join(','));
console.log(count(3).drop(-0.5).toArray().join(','));

// 4: take's close.
console.log(logged('take-close').take(2).toArray().join(','));

// 5: a throwing callback.
console.log(reason(() => logged('map-throw').map(() => { throw new RangeError('m'); }).next()));
console.log(reason(() => logged('filter-throw').filter(() => { throw new RangeError('f'); }).next()));

// 6: `return`, `break`, and a helper that is done.
const h = logged('return').map((x) => x);
console.log(h.next().value);
const stopped = h.return();
console.log(stopped.value, stopped.done);
console.log(h.next().done, h.next().value);
console.log(h.return().done);

const parts = [];
for (const x of logged('break').map((x) => x * 2)) {
  parts.push(x);
  if (x === 4) break;
}
console.log(parts.join(','));

const threw = count(3).map(() => { throw new RangeError('once'); });
console.log(reason(() => threw.next()));
console.log(threw.next().done);

// 7: flatMap's rejected primitives.
console.log(reason(() => logged('flat-string').flatMap(() => 'ab').next()));
console.log(reason(() => count(1).flatMap(() => 7).next()));
console.log(reason(() => count(1).flatMap(() => ({ notAnIterator: true })).next()));

// 8: Iterator.from.
console.log(reason(() => Iterator.from(7)));
console.log(reason(() => Iterator.from(null)));
console.log(reason(() => Iterator.from({ [Symbol.iterator]: 5 })));
console.log(Iterator.from('xy').toArray().join(','));

// 9: the abstract constructor, and a subclass of it.
console.log(reason(() => new Iterator()));
console.log(reason(() => Iterator()));
class Countdown extends Iterator {
  constructor(from) {
    super();
    this.n = from;
  }
  next() {
    return this.n > 0 ? { done: false, value: this.n-- } : { done: true, value: undefined };
  }
}
const down = new Countdown(3);
console.log(down instanceof Iterator, down instanceof Countdown);
console.log(down.map((x) => x * 2).toArray().join(','));

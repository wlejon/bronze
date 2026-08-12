// A generator method as three.js writes one (docs/0026). Every generator in
// three.js 0.160.0 — all six of them, across Vector2/3/4, Euler, Quaternion
// and Color — has exactly the shape below: a `*[Symbol.iterator]()` whose
// body is a straight line of `yield this.<field>;`. bronze desugars that into
// an ordinary method returning an iterator object over a step index, so what
// this case proves is that the desugaring satisfies the protocol docs/0021
// already built rather than a second one beside it.
//
// From ECMA-262 27.5 (generator objects), 7.4.2 (GetIterator), 14.7.5.6
// (ForIn/OfBodyEvaluation), 13.2.4.1 (array spread), 8.6.2 (array
// destructuring) and 15.7.14 (a class method is non-enumerable):
//
// 1. The three consumers of an iterable are one mechanism: `for-of`, spread
//    `[...v]` and `const [a, b, c] = v` all call `@@iterator` and step the
//    result, so all three must agree about the same object.
// 2. Calling `@@iterator` again produces a FRESH walk. A second `[...v]`
//    yields the same three values, and two nested loops over one iterable
//    terminate instead of sharing a cursor.
// 3. Each yielded expression is evaluated when its step is REACHED, not when
//    the iterator is created: writing `v.y` between two walks changes what
//    the second walk yields.
// 4. `this` inside the generator body is the receiver `@@iterator` was
//    called on, which is what makes one method on the prototype serve every
//    instance.
// 5. The method is a class method, so 15.7.14 makes it non-enumerable — and
//    docs/0021 decision 1 makes its name begin with `@@`, which the runtime
//    forces non-enumerable as well. Neither `Object.keys` nor `for-in` may
//    report it.
//
// DELIBERATE DIVERGENCE, docs/0021 decision 1: `Symbol.iterator` is the
// string "@@iterator", so `*[Symbol.iterator]()` is a method named
// "@@iterator" and the key is matched syntactically at compile time.

class Vector3 {
    constructor(x, y, z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    *[ Symbol.iterator ]() {
        yield this.x;
        yield this.y;
        yield this.z;
    }
}

const v = new Vector3(1, 2, 3);

// 1 — the three consumers.
for (const c of v) console.log(c);
console.log([...v]);
const [a, b, c] = v;
console.log(a, b, c);

// 2 — a second walk starts from the beginning.
console.log([...v], [...v]);
for (const p of v) {
    for (const q of v) {
        console.log(p, q);
    }
}

// 3 — the yielded expressions are read per step, not captured up front.
v.y = 20;
console.log([...v]);

// 4 — one prototype method, two receivers.
const w = new Vector3(7, 8, 9);
console.log([...v], [...w]);

// A partial walk abandons the iterator without disturbing the object.
for (const value of v) {
    console.log('first only', value);
    break;
}
console.log([...v]);

// 5 — the method is invisible to enumeration.
console.log(Object.keys(v));
for (const k in v) console.log('key', k);

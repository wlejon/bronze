// What a descriptor OMITS, and what a refusal leaves behind: ECMA-262 6.2.5.6
// CompletePropertyDescriptor and 10.1.6.3 step 4.
//
// An absent field means two different things, and which one is meant depends on
// whether the key already exists. On a NEW key, step 3 completes the descriptor
// — absent `value` becomes `undefined`, absent `writable`, `enumerable` and
// `configurable` become false. On an EXISTING one, step 4 sets only the fields
// the descriptor HAS and leaves the rest of the property exactly as it was, so
// `{ value: 2 }` over an ordinary assignment's property changes the value and
// keeps all three attributes true. Reading the defaults as false either way is
// what would silently freeze a property a program only meant to update.
//
// 6.2.6.1 IsGenericDescriptor is the third answer: `{}` names no data field and
// no accessor field, so it passes every kind test in step 4 — it defines a
// completed data property on a new key and changes nothing but attributes on an
// existing one.
//
// And 20.1.2.3.1 step 5 is a loop of DefinePropertyOrThrow, so a refusal in the
// middle of a block is a TypeError with the earlier keys already defined and
// the later ones never reached. That is not a bronze detail: the operation is
// all-or-nothing about its DECODING (step 4) and strictly in order about its
// defining (step 5).

function attempt(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}

// 1. `{}` on a new key: a data property holding `undefined`, all three
// attributes false. `JSON.stringify` omits a field whose value is `undefined`,
// so the descriptor prints as its three booleans.
const g = {};
Object.defineProperties(g, { p: {} });
console.log('1', 'p' in g, String(g.p),
            JSON.stringify(Object.getOwnPropertyDescriptor(g, 'p')));

// 2. `{ value: v }` on an EXISTING key made by assignment: the value changes
// and the three attributes it does not name stay true.
const e = { q: 1 };
Object.defineProperties(e, { q: { value: 2 } });
console.log('2', e.q, JSON.stringify(Object.getOwnPropertyDescriptor(e, 'q')));

// 3. `{}` on that same existing key: a generic descriptor changes nothing at
// all.
Object.defineProperties(e, { q: {} });
console.log('3', e.q, JSON.stringify(Object.getOwnPropertyDescriptor(e, 'q')));

// 4. A refusal in the middle. `locked` is non-configurable and non-writable, so
// 10.1.6.3 step 4.e refuses a `value` that differs; `first` is already defined
// when that happens and `third` is never reached.
const m = {};
Object.defineProperty(m, 'locked', { value: 'L' });
console.log('4', attempt(() => Object.defineProperties(m, {
    first: { value: 1, enumerable: true },
    locked: { value: 'other' },
    third: { value: 3, enumerable: true },
})), Object.getOwnPropertyNames(m).join(','), m.locked);

// 5. Step 4.e again, the other way: a redefinition that changes NOTHING is
// allowed on a frozen property, because what step 4.e compares is the value
// (7.2.11 SameValue) and not the presence of the field.
const fr = Object.freeze({ n: 1 });
console.log('5', attempt(() => Object.defineProperties(fr, { n: { value: 1 } })), fr.n);
console.log('6', attempt(() => Object.defineProperties(fr, { n: { value: 2 } })), fr.n);

// 7. A frozen object is also not extensible, so a NEW key is refused by step 2
// rather than by step 4.
console.log('7', attempt(() => Object.defineProperties(fr, { z: { value: 1 } })), 'z' in fr);

// 8. The same refusal on an object that is merely non-extensible: its existing
// key still accepts a redefinition.
const ne = Object.preventExtensions({ old: 1 });
console.log('8', attempt(() => Object.defineProperties(ne, { old: { value: 2 } })), ne.old);
console.log('9', attempt(() => Object.defineProperties(ne, { fresh: { value: 1 } })),
            'fresh' in ne);

// 10. `writable: true` alone on a new key: the other two complete to false, so
// the property is writable and invisible.
const w = {};
Object.defineProperties(w, { k: { writable: true } });
w.k = 'set';
console.log('10', w.k, Object.keys(w).length,
            JSON.stringify(Object.getOwnPropertyDescriptor(w, 'k')));

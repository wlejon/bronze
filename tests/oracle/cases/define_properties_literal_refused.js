// The `Object.defineProperties` calls a compiler must NOT read off the source,
// and the answers ECMA-262 gives them anyway.
//
// Each of these is a case where the descriptor map's own text is not the whole
// story. A shadowed `Object` is not the intrinsic. An accessor descriptor is
// decided by 6.2.6.5 steps 7 and 8, which ask whether the field is callable. A
// computed or spread key is not known until the map is built. An ARRAY-INDEX
// key is enumerated by 10.1.11.1 OwnPropertyKeys ahead of the string keys and
// in ascending numeric order, so the defines do not happen in the order the
// source wrote them. A duplicate key leaves the map holding ONE property, whose
// descriptor is the last one written. And a `writable` that is not a boolean is
// a ToBoolean the source did not perform.
//
// The answers here are the ordinary ones; what the case pins is that they stay
// ordinary. A recognition that widened to any of these shapes would change one
// of these lines.

// 1. An accessor descriptor. `get` is a field 6.2.6.5 step 7 reads and step 7.c
// checks for callability, and the property it defines has no `value` and no
// `writable` at all.
const acc = {};
Object.defineProperties(acc, {
    v: { get() { return 'G'; }, enumerable: true, configurable: true },
});
const accDesc = Object.getOwnPropertyDescriptor(acc, 'v');
console.log('1', acc.v, typeof accDesc.get, typeof accDesc.set, accDesc.enumerable,
            accDesc.configurable);

// 2. A local `Object` shadows the intrinsic, and the member called is the
// local's.
function shadowed() {
    const Object = {
        defineProperties(t) {
            t.shadow = 'y';
            return t;
        },
    };
    return JSON.stringify(Object.defineProperties({}, { a: { value: 1 } }));
}
console.log('2', shadowed());

// 3. A computed key beside a literal one: both are own enumerable keys of the
// map, in the order the literal wrote them.
const k = 'comp';
const c = {};
Object.defineProperties(c, {
    [k]: { value: 1, enumerable: true },
    lit: { value: 2, enumerable: true },
});
console.log('3', JSON.stringify(c));

// 4. Array-index keys. 10.1.11.1 lists the integer indices first, ascending,
// then the string keys in insertion order — so the map is walked 2, 10, `two`
// however the source ordered it, and the target ends up with the same order for
// the same reason.
const ix = {};
Object.defineProperties(ix, {
    two: { value: 't', enumerable: true },
    10: { value: 'a', enumerable: true },
    2: { value: 'b', enumerable: true },
});
console.log('4', Object.getOwnPropertyNames(ix).join(','), JSON.stringify(ix));

// 5. A spread contributes the source's own enumerable properties at the point
// it appears.
const src = { sp: { value: 's', enumerable: true } };
const sp = {};
Object.defineProperties(sp, { ...src, own: { value: 'o', enumerable: true } });
console.log('5', JSON.stringify(sp));

// 6. A duplicate key: the map holds one property, and it holds the LAST
// descriptor. The first one is evaluated and then overwritten, so its `value`
// never reaches the target.
const du = {};
Object.defineProperties(du, {
    d: { value: 1, enumerable: true },
    d: { value: 2, enumerable: true },
});
console.log('6', JSON.stringify(du));

// 7. `writable` and `enumerable` that are not booleans: 6.2.6.5 steps 4 and 6
// are ToBoolean, so a non-empty string and 1 are both true.
const tr = {};
Object.defineProperties(tr, { w: { value: 1, writable: 'yes', enumerable: 1 } });
console.log('7', JSON.stringify(Object.getOwnPropertyDescriptor(tr, 'w')));

// 8. `__proto__` in the map is 13.2.5.5's prototype assignment and not a key, so
// the map has one own key and the target gets one property.
const pr = {};
Object.defineProperties(pr, {
    __proto__: null,
    keep: { value: 'k', enumerable: true },
});
console.log('8', JSON.stringify(pr), Object.getOwnPropertyNames(pr).join(','));

// 9. A descriptor that is not an object at all: 6.2.6.5 step 1.
function attempt(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}
console.log('9', attempt(() => Object.defineProperties({}, { a: 5 })));

// 10. A field the decode never reads is simply not part of the descriptor.
const extra = {};
Object.defineProperties(extra, { a: { value: 7, enumerable: true, nonsense: 1 } });
console.log('10', JSON.stringify(Object.getOwnPropertyDescriptor(extra, 'a')));

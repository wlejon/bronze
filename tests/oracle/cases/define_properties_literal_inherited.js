// A descriptor literal does not say what its descriptor CONTAINS: ECMA-262
// 6.2.6.5 reads each of the six fields with HasProperty and Get, and both walk
// the prototype chain.
//
// So `{ value: 1 }` is a non-enumerable, non-writable, non-configurable
// property only while nothing on `Object.prototype` answers those names. A
// program that puts one there has changed what every descriptor literal in it
// means — including `Object.prototype.get`, which does not even have to be a
// function to matter: step 7.c rejects a `get` that is present and is neither
// callable nor `undefined`, so an inherited number makes every plain data
// descriptor a TypeError.
//
// It is an obscure program and a decisive one: reading a descriptor off its
// source text is only sound while the chain above it is silent, and this is
// where that condition is written down.

function attempt(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}

// 1. An inherited `enumerable`, which the literal never wrote.
Object.prototype.enumerable = true;
const a = {};
Object.defineProperties(a, { k: { value: 1 } });
console.log('1', Object.keys(a).join(','),
            JSON.stringify(Object.getOwnPropertyDescriptor(a, 'k')));

// 2. An OWN field shadows the inherited one, so a literal that writes the field
// is unaffected.
const b = {};
Object.defineProperties(b, { k: { value: 1, enumerable: false } });
console.log('2', Object.keys(b).length,
            JSON.stringify(Object.getOwnPropertyDescriptor(b, 'k')));

delete Object.prototype.enumerable;

// 3. And removing it puts the default back.
const c = {};
Object.defineProperties(c, { k: { value: 1 } });
console.log('3', JSON.stringify(Object.getOwnPropertyDescriptor(c, 'k')));

// 4. An inherited `get` that is not callable: 6.2.6.5 step 7.c, raised before
// 10.1.6.3 runs at all, so nothing is defined.
Object.prototype.get = 5;
const d = {};
console.log('4', attempt(() => Object.defineProperties(d, { k: { value: 1 } })), 'k' in d);
delete Object.prototype.get;

// 5. An inherited `writable`, which is the one of the three that is observable
// without asking for a descriptor.
Object.prototype.writable = true;
const e = {};
Object.defineProperties(e, { k: { value: 1 } });
e.k = 2;
console.log('5', e.k, JSON.stringify(Object.getOwnPropertyDescriptor(e, 'k')));
delete Object.prototype.writable;

// 6. With the chain silent again, the same literal describes what it says.
const f = {};
Object.defineProperties(f, { k: { value: 1, enumerable: true } });
f.k = 2;
console.log('6', f.k, JSON.stringify(Object.getOwnPropertyDescriptor(f, 'k')));

// `Object.defineProperties(o, { k: { ... }, ... })` with descriptors written as
// object literals: ECMA-262 20.1.2.3, 20.1.2.3.1, and the attributes 10.1.6.3
// gives a key the descriptor did not fully describe.
//
// This is the shape three.js gives every `Object3D`, and every fact it depends
// on is an attribute rather than a value. A descriptor that names
// `configurable` and `enumerable` and `value` leaves `writable` ABSENT, and
// 10.1.6.3 step 3 completes an absent field on a NEW property with false — so
// `position` is enumerable and configurable and NOT writable. A descriptor that
// names only `value` leaves all three absent, so `modelViewMatrix` is a data
// property nothing can see, change or remove: it is not in `Object.keys`, not
// in `JSON.stringify`, not in `for-in`, an assignment to it fails (silently in
// sloppy code, as a TypeError in strict), and `delete` refuses it.
//
// So the descriptor's OMISSIONS are the observable content, which is exactly
// what a lowering that reads the literal instead of building it has to carry.
// "absent" and "present and false" agree here — the property is new — and
// disagree on a redefinition, which `define_properties_literal_defaults` pins.

function attempt(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}

const position = { x: 1 };
const scale = { s: 2 };
const o = {};
Object.defineProperties(o, {
    position: { configurable: true, enumerable: true, value: position },
    scale: { configurable: true, enumerable: true, value: scale },
    modelViewMatrix: { value: 'M' },
    normalMatrix: { value: 'N' },
});

// 1-3. Enumerability, three ways of asking. `getOwnPropertyNames` is the one
// that does not filter, so it is the one that shows the other two are filtering.
console.log('1', Object.keys(o).join(','));
console.log('2', Object.getOwnPropertyNames(o).join(','));
console.log('3', JSON.stringify(o));

// 4-5. 6.2.6.4 FromPropertyDescriptor, whose field order is the specification's:
// value, writable, enumerable, configurable for a data property.
console.log('4', JSON.stringify(Object.getOwnPropertyDescriptor(o, 'position')));
console.log('5', JSON.stringify(Object.getOwnPropertyDescriptor(o, 'modelViewMatrix')));

// 6. for-in walks the prototype chain, and filters by the same bit `Object.keys`
// does.
const seen = [];
for (const k in o) seen.push(k);
console.log('6', seen.join(','));

// 7. 6.2.5.5/10.1.9.2: a sloppy assignment to a non-writable property is a
// refusal that returns, not one that throws.
o.modelViewMatrix = 'X';
console.log('7', o.modelViewMatrix);

// 8-9. `delete` answers the [[Configurable]] bit (13.5.1.2 through 10.1.10.1),
// so the two keys of this object give opposite answers.
console.log('8', delete o.modelViewMatrix, o.modelViewMatrix);
console.log('9', delete o.position, 'position' in o);

// 10. The same refusal as 7, in strict code, where 6.2.5.5 step 6 makes it a
// TypeError instead.
function strictWrite() {
    'use strict';
    o.normalMatrix = 'Y';
}
console.log('10', attempt(strictWrite), o.normalMatrix);

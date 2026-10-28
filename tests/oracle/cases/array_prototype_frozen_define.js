// A FROZEN `Array.prototype` — what embed::lockdown leaves every intrinsic,
// and what `Object.freeze` makes of it here — answers each change the way
// ECMA-262 answers any frozen object: 20.1.2.4 Object.defineProperty and
// 20.1.2.3 defineProperties throw (DefinePropertyOrThrow), 28.1.3
// Reflect.defineProperty answers false, a define that changes nothing still
// succeeds (10.1.6.3 step 4.e), a sloppy assignment is dropped and a strict one
// throws, `delete` answers false, and [[SetPrototypeOf]] refuses. Whatever the
// spelling — a class `static` block, `Object.assign` — the call path must reach
// the same refusal, and the methods arrays answer stay the ones they had.
//
// An UNFROZEN `Array.prototype` is bronze's own refusal (it cannot be decorated:
// rt_builtins.h) and is pinned in tests/eval/eval_lockdown_test.cpp, since node
// accepts those changes.

const shift = Array.prototype.shift;
Object.freeze(Array.prototype);
console.log('frozen', Object.isFrozen(Array.prototype), Object.isExtensible(Array.prototype));

const throws = (what, f) => {
    try {
        f();
        console.log(what, 'no throw');
    } catch (e) {
        console.log(what, e instanceof TypeError ? 'TypeError' : 'other: ' + e);
    }
};

// 1. The descriptor family over an existing method, a new key and an accessor.
throws('1 defineProperty', () => Object.defineProperty(Array.prototype, 'shift', { value() { return 7; } }));
throws('1 defineProperty new', () => Object.defineProperty(Array.prototype, 'fresh', { value: 1 }));
throws('1 defineProperty accessor', () => Object.defineProperty(Array.prototype, 'acc', { get() { return 4; } }));
throws('1 defineProperties', () => Object.defineProperties(Array.prototype, { pop: { value: 5 } }));
throws('1 defineProperties literal', () => Object.defineProperties(Array.prototype, { push: { value: 5, writable: true } }));
throws('1 static block', () => {
    class C {
        static {
            Object.defineProperty(Array.prototype, 'map', { value: null });
        }
    }
    return C;
});

// 2. Reflect.defineProperty answers the boolean instead, and never throws for a
// refusal.
console.log('2', Reflect.defineProperty(Array.prototype, 'shift', { value() { return 7; } }),
            Reflect.defineProperty(Array.prototype, 'fresh', { value: 1 }),
            Reflect.defineProperty(Array.prototype, 'shift', { writable: true }),
            Reflect.defineProperty(Array.prototype, 'shift', { enumerable: true }));

// 3. A redefinition that changes nothing is not a refusal (10.1.6.3 step 4.e).
console.log('3', Reflect.defineProperty(Array.prototype, 'shift', { value: shift }),
            Reflect.defineProperty(Array.prototype, 'length', { value: 0, writable: false }),
            Object.defineProperty(Array.prototype, 'shift', { value: shift, enumerable: false }) === Array.prototype);

// 4. Assignment, by name, by key and through Object.assign / Reflect.set.
Array.prototype.push = () => 99;
Array.prototype['po' + 'p'] = () => 99;
console.log('4 sloppy', typeof Array.prototype.push, typeof Array.prototype.pop);
throws('4 strict', () => { 'use strict'; Array.prototype.push = () => 99; });
throws('4 strict new', () => { 'use strict'; Array.prototype.brandNew = 1; });
throws('4 assign', () => Object.assign(Array.prototype, { map() { return 1; } }));
console.log('4 Reflect.set', Reflect.set(Array.prototype, 'shift', 1),
            Reflect.set({}, 'shift', 1, Array.prototype));

// 5. delete and the prototype.
console.log('5 delete', delete Array.prototype.shift, Reflect.deleteProperty(Array.prototype, 'map'),
            delete Array.prototype.absent);
throws('5 strict delete', () => { 'use strict'; delete Array.prototype.shift; });
throws('5 setPrototypeOf', () => Object.setPrototypeOf(Array.prototype, {}));
console.log('5 Reflect.setPrototypeOf', Reflect.setPrototypeOf(Array.prototype, {}),
            Reflect.setPrototypeOf(Array.prototype, Object.prototype));

// 6. Nothing changed, for the object or for an array.
console.log('6', Array.prototype.shift === shift, 'fresh' in Array.prototype, 'acc' in Array.prototype,
            [1, 2].shift(), [1, 2].pop(), [3].push(4), [1, 2].map(x => x * 2).join());
const d = Object.getOwnPropertyDescriptor(Array.prototype, 'shift');
console.log('6', d.writable, d.enumerable, d.configurable, typeof d.value);

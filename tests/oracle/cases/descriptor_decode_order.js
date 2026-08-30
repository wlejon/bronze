// Reading a descriptor OBJECT: ECMA-262 6.2.6.5 ToPropertyDescriptor, and the
// two answers 10.1.6.3 gives that depend on reading it exactly.
//
// A descriptor is an ordinary object, so each of its six fields may be a
// GETTER, and then the decode is a program the descriptor's author wrote.
// 6.2.6.5 fixes the order that program runs in — enumerable, configurable,
// value, writable, get, set — and the order is observable twice over: a getter
// can record that it ran, and a getter can add or change a field the decode has
// not reached yet. So the order is pinned bytes and not an implementation
// convenience.
//
// Steps 7.c and 8.c then reject a `get` or `set` that is present and is neither
// callable nor `undefined`: a number cannot be called, so a descriptor naming
// one does not describe an accessor and nothing is defined.
//
// Every field read is spelled `? HasProperty(...)` / `? Get(...)`, and the `?`
// is load-bearing: a field getter that throws is an abrupt completion, so
// 6.2.6.5 returns THERE. The fields after it are never read — their getters do
// not run — and 10.1.6.3 is never reached, so the property is not defined at
// all. A decode that carried on with the `undefined` the failed read left
// behind defined the property anyway, which is the worst shape this file has:
// the program saw its own error propagate and the object changed regardless.
//
// 10.1.6.3 step 4 decides what a NON-CONFIGURABLE property still accepts, and
// it decides by COMPARING the fields present against the ones in place — with
// 7.2.11 SameValue, which is `===` with two corrections: NaN is the same value
// as NaN, and +0 is not the same value as -0. A redefinition that changes
// nothing is allowed, which is what makes a frozen object's own value a legal
// thing to write back.
//
// And 28.1.3 Reflect.defineProperty returns THE BOOLEAN [[DefineOwnProperty]]
// answered, where 20.1.2.4 Object.defineProperty raises a TypeError for the
// same refusal and returns the target for the same success. That is the whole
// reason the Reflect member exists: a program that wants to know whether the
// define landed cannot ask a member that throws. What it still throws is what
// happens BEFORE the define — a target that is not an object, and a descriptor
// the decode above rejects.

function attempt(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}

// 1. The four attribute-and-value fields as getters, each recording its turn.
// 6.2.6.5 steps 3, 4, 5 and 6, in that order.
const order = [];
const spy = {
    get value() { order.push('value'); return 1; },
    get writable() { order.push('writable'); return true; },
    get enumerable() { order.push('enumerable'); return true; },
    get configurable() { order.push('configurable'); return true; },
};
Object.defineProperty({}, 'p', spy);
console.log('1', order.join(','));

// 2. `get` and `set` are steps 7 and 8, so they come LAST — after the two
// attributes and after the pair they are mutually exclusive with.
const order2 = [];
const spy2 = {
    get set() { order2.push('set'); return undefined; },
    get get() { order2.push('get'); return function () { return 'G'; }; },
    get enumerable() { order2.push('enumerable'); return true; },
    get configurable() { order2.push('configurable'); return true; },
};
const o2 = {};
Object.defineProperty(o2, 'q', spy2);
console.log('2', order2.join(','), o2.q);

// 3. A getter for an EARLY field that adds a LATE one is seen, because the
// late field has not been read yet.
const late = { get enumerable() { late.value = 'late'; return true; } };
const o3 = {};
Object.defineProperty(o3, 'p', late);
console.log('3', String(o3.p), Object.getOwnPropertyDescriptor(o3, 'p').enumerable);

// 4. And the mirror: a getter for a LATE field that adds an EARLY one is not,
// because that field was asked for and answered absent two steps ago.
const early = { get value() { early.enumerable = true; return 'v'; } };
const o4 = {};
Object.defineProperty(o4, 'p', early);
console.log('4', o4.p, Object.getOwnPropertyDescriptor(o4, 'p').enumerable);

// 5-9. Steps 7.c and 8.c: present, not callable, not undefined -> TypeError.
// `undefined` is the one non-callable value both fields accept, because that
// is how an accessor spells a half it does not have.
console.log('5', attempt(() => Object.defineProperty({}, 'p', { get: 1 })));
console.log('6', attempt(() => Object.defineProperty({}, 'p', { set: 'x' })));
console.log('7', attempt(() => Object.defineProperty({}, 'p', { get: undefined, set: undefined })));
console.log('8', attempt(() => Object.defineProperty({}, 'p', { get: null })));
console.log('9', attempt(() => Object.defineProperty({}, 'p', { get: 1, value: 2 })));

// 10-21. 10.1.6.3 step 4 over a frozen object, which is non-configurable and
// non-writable in every property. Step 4.e.ii compares the value with
// SameValue, so the same value written back is not a change and is allowed;
// 4.e.i refuses the promotion back to writable; 4.a refuses configurable; 4.b
// refuses an enumerable that differs and allows one that does not; 4.c refuses
// the change of kind.
const madeS = ['a', 'b', 'c'].join('');
const fr = Object.freeze({ n: 1, s: 'abc', z: 0, nan: NaN });
console.log('10', attempt(() => Object.defineProperty(fr, 'n', { value: 1 })));
console.log('11', attempt(() => Object.defineProperty(fr, 'n', { value: 2 })));
console.log('12', attempt(() => Object.defineProperty(fr, 's', { value: madeS })));
console.log('13', attempt(() => Object.defineProperty(fr, 'nan', { value: NaN })));
console.log('14', attempt(() => Object.defineProperty(fr, 'z', { value: -0 })));
console.log('15', attempt(() => Object.defineProperty(fr, 'z', { value: 0 })));
console.log('16', attempt(() => Object.defineProperty(fr, 'n', { writable: true })));
console.log('17', attempt(() => Object.defineProperty(fr, 'n', { configurable: true })));
console.log('18', attempt(() => Object.defineProperty(fr, 'n', { enumerable: true })));
console.log('19', attempt(() => Object.defineProperty(fr, 'n', { enumerable: false })));
console.log('20', attempt(() => Object.defineProperty(fr, 'n', { get() { return 1; } })));
console.log('21', fr.n, fr.s, fr.z, String(fr.nan));

// 22-29. 28.1.3, and what separates it from 20.1.2.4: a refusal is `false` and
// not a throw, a success is `true` and not the target, and the errors of the
// decode still raise for both.
const rt = { p: 1 };
console.log('22', Reflect.defineProperty(rt, 'p', { value: 2 }), rt.p);
const rf = Object.freeze({ p: 1 });
console.log('23', Reflect.defineProperty(rf, 'p', { value: 3 }), rf.p);
console.log('24', Reflect.defineProperty(rf, 'p', { value: 1 }));
const ne = Object.preventExtensions({});
console.log('25', Reflect.defineProperty(ne, 'fresh', { value: 1 }), 'fresh' in ne);
console.log('26', attempt(() => Reflect.defineProperty(rf, 'p', { get: 1 })));
console.log('27', attempt(() => Reflect.defineProperty(5, 'p', { value: 1 })));
console.log('28', attempt(() => Reflect.defineProperty({}, 'p', 5)));
console.log('29', Reflect.defineProperty({}, 'p', { value: 1 }));

// 30-33. The abrupt completion. A RangeError rather than a TypeError so that
// the error under test cannot be confused with one 6.2.6.5 or 10.1.6.3 raises
// on their own.
function attemptRange(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof RangeError ? 'RangeError' : 'other';
    }
}

// 30. A getter that throws at step 5, after step 3 has already answered. The
// error propagates and NOTHING is defined — not even the property carrying the
// `enumerable` the decode had already read.
const d30 = {};
console.log('30', attemptRange(() => Object.defineProperty(d30, 'p', {
    get enumerable() { return true; },
    get value() { throw new RangeError('mid'); },
})), 'p' in d30, Object.getOwnPropertyDescriptor(d30, 'p') === undefined);

// 31. The same at the FIRST read, which after the order above is `enumerable`.
// `reached31` is the half a `?` alone would not prove: the later fields are not
// merely ignored, their getters never run.
const d31 = {};
let reached31 = false;
console.log('31', attemptRange(() => Object.defineProperty(d31, 'p', {
    get enumerable() { throw new RangeError('first'); },
    get value() { reached31 = true; return 1; },
})), 'p' in d31, reached31);

// 32. 20.1.2.3.1 step 4 decodes descriptors in key order and stops at the first
// abrupt completion, so `c`'s getter never runs and neither `b` nor `c` is
// defined. Whether `a` — decoded successfully before the throw — is defined is
// step 4-versus-step-5 and is pinned in `blocked/descriptor_define_all_or_
// nothing`, which bronze does not answer correctly yet.
const d32 = {};
const reads32 = [];
console.log('32', attemptRange(() => Object.defineProperties(d32, {
    a: { get value() { reads32.push('a'); return 1; }, enumerable: true },
    b: { get value() { reads32.push('b'); throw new RangeError('second'); } },
    c: { get value() { reads32.push('c'); return 3; } },
})), 'b' in d32, 'c' in d32, reads32.join(','));

// 33. 28.1.3 returns a boolean for a REFUSAL, but an abrupt completion out of
// the decode is not a refusal: it propagates, and the call returns nothing.
const d33 = {};
console.log('33', attemptRange(() => Reflect.defineProperty(d33, 'p', {
    get value() { throw new RangeError('reflect'); },
})), 'p' in d33);

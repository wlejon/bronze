// Storing a property on a PRIMITIVE receiver, and the one place bronze answers
// a refusal differently in strict and sloppy code.
//
// ECMA-262 13.15.2 PutValue over a property reference whose base is a
// primitive: step 6 boxes the base with 7.1.18 ToObject, step 7 calls the box's
// [[Set]] with the PRIMITIVE as the receiver, and step 8 throws a TypeError
// only if the reference is strict. 10.1.9.2 OrdinarySetWithOwnDescriptor step
// 2.b is what makes the answer false: a data write creates an own property of
// the RECEIVER, and a receiver that is not an object cannot have one. The box
// is thrown away at the end of the statement, so nothing was ever storable —
// which is why discarding the write here is not a lie a later read can catch
// out. A read of the same name afterwards is `undefined`, and that is the
// state.
//
// `null` and `undefined` are a different failure and stay loud in both modes:
// step 6's ToObject has no answer for them at all (7.1.18 table 14), so the
// TypeError is raised before any [[Set]] is reached.
//
// Two spellings, one operation: `p.k = v` and `p[k] = v` differ only in when
// the key is known, and a refusal built into one of them is not a refusal.
//
// Step 3 is the part that makes this a real [[Set]] on the box rather than a
// discard decided by the receiver's kind: an ACCESSOR anywhere on the box's
// prototype chain still runs, and its `this` is the primitive.

function attempt(fn) {
    try {
        return fn();
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}
function put(o, k, v) { o[k] = v; return o; }
function putStrict(o, k, v) { 'use strict'; o[k] = v; return 'stored'; }

// 1-5. A named sloppy store on each primitive kind: nothing raised, nothing
// stored, the value itself untouched.
const st = 'abc';
st.x = 1;
console.log('1', String(st.x), st, st.length);

const nu = 5;
nu.x = 1;
console.log('2', String(nu.x), nu);

const bo = true;
bo.x = 1;
console.log('3', String(bo.x), bo);

const bi = 7n;
bi.x = 1;
console.log('4', String(bi.x), String(bi));

const sy = Symbol('s');
sy.x = 1;
console.log('5', String(sy.x), typeof sy);

// 6. The computed spelling of the same three stores.
console.log('6', String(put('abc', 'x', 1).x), String(put(5, 'x', 1).x),
            String(put(true, 'x', 1).x));

// 7-10. Strict: step 8 turns the same false into a TypeError, in both
// spellings.
console.log('7', attempt(() => { 'use strict'; const t = 'abc'; t.x = 1; return 'stored'; }));
console.log('8', attempt(() => putStrict('abc', 'x', 1)));
console.log('9', attempt(() => { 'use strict'; const t = 5; t.x = 1; return 'stored'; }));
console.log('10', attempt(() => putStrict(true, 'x', 1)));

// 11-14. `null` and `undefined` throw in BOTH modes: ToObject fails before
// [[Set]] is ever consulted, so there is no false for the strict flag to
// choose between.
console.log('11', attempt(() => { const z = null; z.x = 1; return 'stored'; }));
console.log('12', attempt(() => { 'use strict'; const z = null; z.x = 1; return 'stored'; }));
console.log('13', attempt(() => put(undefined, 'x', 1)));
console.log('14', attempt(() => putStrict(undefined, 'x', 1)));

// 15-17. A String box's own `length` (10.4.3.4) and its indices (10.4.3.5) are
// non-writable OWN properties, so those refusals are the box's rather than the
// receiver's — but they are refusals all the same, and the strict/sloppy split
// is the same one.
const w = 'abc';
w[0] = 'z';
w.length = 9;
console.log('15', w, w.length, w[0]);
console.log('16', attempt(() => { 'use strict'; const v = 'abc'; v[0] = 'z'; return 'stored'; }));
console.log('17', attempt(() => putStrict('abc', 'length', 9)));

// 18. A name the chain does not hold reads as absent afterwards, which is the
// state a discarded store leaves.
console.log('18', String((5).nothing), String('abc'.nothing));

// 19-20. And step 3: a setter on the intrinsic prototype IS reached, with the
// primitive as the value it is handed. This is what a receiver-kind shortcut
// would silently skip.
let seen = '';
Object.defineProperty(Number.prototype, 'probe', {
    set(v) { seen = 'set:' + String(v); },
    configurable: true,
});
const nn = 42;
nn.probe = 'hit';
console.log('19', seen);
seen = '';
put(nn, 'probe', 'again');
console.log('20', seen);

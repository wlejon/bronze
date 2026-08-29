// The ARRAY STORE-RUN RECEIVER PROOF
// (src/codegen-llvm/llvm_array_store_proof.h): a run of `arr[k] = v` writes at
// constant indices that proves the receiver, the bound, the head offset and the
// absence of a named-properties side object ONCE and then spends that proof on
// each member.
//
// Everything below is a question about what the language says, not about what
// the proof does — the proof is only sound if turning it off changes nothing.
// The spec sections each group pins:
//
//   10.4.2.1  Array [[DefineOwnProperty]]: an index at or past `length` still
//             succeeds and RAISES `length` to index + 1, leaving every index
//             between the old length and the new one a hole.
//   10.1.9.2  OrdinarySetWithOwnDescriptor: a write to a non-writable own data
//             property returns false, which 13.15.2 PutValue step 6.d turns
//             into a TypeError for a STRICT reference and into nothing at all
//             for a sloppy one. `Object.freeze` is what makes an element
//             non-writable; `Object.seal` does not.
//   10.1.6.3  Ordinary [[DefineOwnProperty]]: creating a property — which is
//             what filling a HOLE below `length` is — needs [[Extensible]].
//   10.1.11.1 OrdinaryOwnPropertyKeys: array-index keys ascending first, then
//             string keys in creation order.
//   6.1.7     An array index is a canonical numeric string, so `a['01']` is a
//             NAMED property and not element 1.
//   13.15.2   AssignmentExpression: the value is stored as it is. A property
//             write performs NO coercion of the value, so an object with a
//             `valueOf` is stored whole and `valueOf` is not called.
//
// This file is a Script and therefore SLOPPY (module code alone is always
// strict, 11.2.2), which is what lets the frozen-array group pin both halves of
// PutValue step 6.d in one program.

function dump(a) {
  let s = '';
  for (let i = 0; i < a.length; i++) {
    if (i > 0) s += ',';
    s += a[i];
  }
  return s;
}

// The run the proof exists for, four wide, with a gap: indices 0, 1, 2 and 5,
// so the ONE bound the proof takes is 5 and a receiver shorter than that
// refuses the whole run rather than half of it.
function put4(a, v0, v1, v2, v3) {
  a[0] = v0;
  a[1] = v1;
  a[2] = v2;
  a[5] = v3;
  return a;
}

// --- 1. The shape all of this exists for: three.js's Matrix4.copy, sixteen
// constant-index reads off one array interleaved with sixteen constant-index
// writes into another. Before this proof the first WRITE ended the read run
// standing over it, so both halves paid their ladder sixteen times.
class M4 {
  constructor(e) {
    this.elements = e;
  }
  copy(m) {
    const te = this.elements;
    const me = m.elements;
    te[0] = me[0];
    te[1] = me[1];
    te[2] = me[2];
    te[3] = me[3];
    te[4] = me[4];
    te[5] = me[5];
    te[6] = me[6];
    te[7] = me[7];
    te[8] = me[8];
    te[9] = me[9];
    te[10] = me[10];
    te[11] = me[11];
    te[12] = me[12];
    te[13] = me[13];
    te[14] = me[14];
    te[15] = me[15];
    return this;
  }
  scale(s) {
    const te = this.elements;
    te[0] = te[0] * s;
    te[1] = te[1] * s;
    te[2] = te[2] * s;
    te[3] = te[3] * s;
    return this;
  }
}

const source = new M4([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
const dest = new M4([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
console.log('copy: ' + dump(dest.copy(source).elements) +
            ' len=' + dest.elements.length);

// The receiver ALIASED to the source. Every member reads and writes the same
// element, so the copy is the identity — and a proof that had gone stale
// against its own writes would not say so.
console.log('self: ' + dump(source.copy(source).elements));

// A read-modify-write run: the value stored is arithmetic over a read of the
// SAME array the run writes.
console.log('scale: ' + dump(dest.scale(2).elements));

// --- 2. Store-then-load ordering inside one array. Each line reads what the
// line before it wrote, so a read hoisted over a write would show through.
function chain(te) {
  te[0] = te[1];
  te[1] = te[2];
  te[2] = te[0];
  return te;
}
console.log('order: ' + dump(chain([1, 2, 3])));

// --- 3. A run whose largest index is past the receiver's `length`. The one
// bound covers the whole run, so the proof is refused and every member takes
// the ladder — which is where 10.4.2.1 lives: `a[5] = v` on a length-3 array
// stores at 5 and raises `length` to 6, leaving 3 and 4 as holes.
const short3 = put4([0, 0, 0], 1, 2, 3, 4);
console.log('short: ' + dump(short3) + ' len=' + short3.length +
            ' keys=' + Object.keys(short3).join('|') + ' has3=' + (3 in short3));

// The same run into an array long enough for it: the proof holds, and the
// answer is the one the ladder gives.
console.log('long: ' + dump(put4([0, 0, 0, 0, 0, 0, 0, 0], 1, 2, 3, 4)));

// --- 4. A HEAD-OFFSET array — one `shift()` left behind. Element k lives at
// slot head + k, which is why the proof reads the head and tests it against the
// capacity rather than assuming zero.
const shifted = [9, 1, 2, 3, 4, 5, 6, 7];
shifted.shift();
console.log('shifted: ' + dump(put4(shifted, 100, 200, 300, 400)) +
            ' len=' + shifted.length);

// --- 5. A HOLE inside the run. `delete` takes index 2 out of `Object.keys`,
// `for-in` and `in` without moving `length`; the run's store at 2 is therefore
// a CREATE (10.1.6.3 needs [[Extensible]]) and puts it back.
const sparse = [1, 2, 3, 4, 5, 6];
delete sparse[2];
console.log('hole: has2=' + (2 in sparse) + ' keys=' + Object.keys(sparse).length +
            ' at2=' + sparse[2]);
put4(sparse, 10, 20, 30, 60);
console.log('filled: ' + dump(sparse) + ' has2=' + (2 in sparse) +
            ' keys=' + Object.keys(sparse).join('|'));

// --- 6. A FROZEN array. Freezing makes every element non-writable, and it
// records that in the array's named-properties side object — which is the very
// thing the proof tests for, so a frozen array can never take the fast arm.
// 10.1.9.2 returns false and 13.15.2 discards for a sloppy reference.
const frozen = Object.freeze([1, 2, 3, 4, 5, 6]);
put4(frozen, 10, 20, 30, 60);
console.log('frozen-sloppy: ' + dump(frozen));

// The same writes through a STRICT reference: PutValue step 6.d throws.
function putStrict(a) {
  'use strict';
  a[0] = 10;
  a[1] = 20;
  a[2] = 30;
  a[5] = 60;
}
let frozenThrew = '';
try {
  putStrict(frozen);
} catch (e) {
  frozenThrew = (e instanceof TypeError) ? 'TypeError' : 'other';
}
console.log('frozen-strict: ' + dump(frozen) + ' threw=' + frozenThrew);

// A SEALED array. Sealing makes elements non-configurable, not non-writable,
// so every write to an index that already exists lands.
const sealed = Object.seal([1, 2, 3, 4, 5, 6]);
put4(sealed, 10, 20, 30, 60);
console.log('sealed: ' + dump(sealed));

// --- 7. An array carrying a NAMED property. The side object exists, so the
// proof is refused and every member takes the ladder — and the answer does not
// change.
const named = [1, 2, 3, 4, 5, 6];
named.foo = 1;
put4(named, 10, 20, 30, 60);
console.log('named: ' + dump(named) + ' foo=' + named.foo);

// `a['01']` is not an array index (6.1.7: ToString(1) is "1", not "01"), so it
// is a named property and `length` does not move.
const canon = [1, 2, 3];
canon['01'] = 9;
console.log('canonical: ' + dump(canon) + ' len=' + canon.length +
            ' keys=' + Object.keys(canon).join('|'));

// --- 8. A `length`-shrunk array. Shrinking deletes the dropped elements, so
// the run is past the end again and 10.4.2.1 grows `length` back with holes
// where nothing was written.
const shrunk = [1, 2, 3, 4, 5, 6];
shrunk.length = 2;
put4(shrunk, 10, 20, 30, 60);
console.log('shrunk: ' + dump(shrunk) + ' len=' + shrunk.length +
            ' keys=' + Object.keys(shrunk).join('|'));

// --- 9. Receivers that are not Arrays at all. Each takes a path the proof does
// not cover, and each must answer exactly what it answered before.
console.log('typed: ' + dump(put4(new Float64Array(8), 1, 2, 3, 4)));

const obj = put4({}, 'a', 'b', 'c', 'd');
console.log('object: ' + Object.keys(obj).join('|') + ' ' + obj[0] + obj[1] + obj[2] + obj[5]);

function viaArguments() {
  put4(arguments, 10, 20, 30, 60);
  return dump(arguments) + ' len=' + arguments.length;
}
console.log('arguments: ' + viaArguments(1, 2, 3, 4, 5, 6));

// --- 10. A store performs NO coercion. The value is an object with a
// `valueOf`, and storing it must not call it — which is why the fast arm has no
// value test at all, unlike the typed-array one.
const calls = [];
const boxy = {
  valueOf: function () {
    calls.push('valueOf');
    return 7;
  }
};
const holder = put4([0, 0, 0, 0, 0, 0], boxy, 1, 2, 3);
console.log('nocoerce: same=' + (holder[0] === boxy) + ' calls=' + calls.length +
            ' plus1=' + (holder[0] + 1) + ' after=' + calls.length);

// --- 11. A source with GETTERS that write into the target mid-run. A read
// whose fast arm is not taken reaches bronze_prop_get, which runs the getter,
// which can collect — so the store proof cannot survive that member. What is
// pinned is the observable ORDER: index 0 is written with 10, the getter then
// overwrites it with 99 and that survives, and the getter at index 3 writes
// index 2 before the run's own store to index 3.
function copy4(dst, src) {
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
  return dst;
}
const target11 = [0, 0, 0, 0];
const src11 = {
  '0': 10,
  get '1'() {
    target11[0] = 99;
    return 11;
  },
  '2': 12,
  get '3'() {
    target11[2] = 77;
    return 13;
  }
};
console.log('getter: ' + dump(copy4(target11, src11)));

// --- 12. A getter that ALLOCATES mid-run. The proof's base is derived into the
// elements block, which a collection moves, so the members after it must store
// into the block the array has NOW.
let sink = null;
const target12 = [0, 0, 0, 0];
const src12 = {
  '0': 1,
  get '1'() {
    sink = [];
    for (let i = 0; i < 300; i++) sink.push({ i: i, s: 'x' + i });
    return 2;
  },
  '2': 3,
  '3': 4
};
console.log('alloc: ' + dump(copy4(target12, src12)) + ' sink=' + sink.length);

// --- 13. A getter that THROWS mid-run. Everything stored before it is visible
// after the catch; nothing after it ran.
const target13 = [0, 0, 0, 0];
const src13 = {
  '0': 1,
  '1': 2,
  get '2'() {
    throw new Error('boom');
  },
  '3': 4
};
let caught = '';
try {
  copy4(target13, src13);
} catch (e) {
  caught = e.message;
}
console.log('throwmid: ' + dump(target13) + ' caught=' + caught);

// --- 14. A CALL inside the run. A call can move the heap, so the run stops
// there and a second one starts after it; both halves must land.
let ticks = 0;
function putWithCall(a, f) {
  a[0] = 1;
  a[1] = 2;
  f();
  a[2] = 3;
  a[3] = 4;
  return a;
}
console.log('split: ' + dump(putWithCall([0, 0, 0, 0], function () {
  ticks++;
})) + ' ticks=' + ticks);

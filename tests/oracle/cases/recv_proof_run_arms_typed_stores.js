// A SPAN THAT STORES INTO A TYPED ARRAY, EMITTED AS TWO ARMS BEHIND A GATE
// (src/codegen-llvm/llvm_run_arms.h).
//
// The shape is three.js's `Matrix4.toArray`: sixteen constant-index reads off
// one Array threaded through sixteen affine-index writes into a typed array.
// A typed-array store owes ToNumber, and ToNumber runs user code — so the
// store's proven arm has always tested its VALUE, and that test is not known
// at the head of the span. The GATE is where it becomes known: the loads and
// the arithmetic the stored values are made of run first, every one of them is
// tested for being a Number, and only then does the arm that stores anything
// begin. A refusal at the gate has stored NOTHING, so the slow arm performs
// the whole span from the top.
//
// What that has to leave unchanged, and what each group below pins:
//
//   THE VALUES. A non-number anywhere in the span sends all sixteen stores
//   down the ladder, and the ladder's conversion is the language's:
//   ToNumber of a string, of `true`, of `null`, of `undefined` (NaN), of a
//   hole (NaN), and of an object through its `valueOf`.
//
//   EXACTLY ONCE. `valueOf` is called once, not once at the gate and again on
//   the arm. The gate loads elements and tests bits; it converts nothing.
//
//   THE BOUND. The store run's one length test covers the LARGEST offset, so a
//   destination that cannot hold `offset + 15` refuses the proof and every
//   member decides for itself — and an out-of-range typed-array store is a
//   discard (10.4.5.5), never a throw and never a named property.
//
//   THE KINDS. FLOAT32 narrows, FLOAT64 does not, and INT32 is not a kind the
//   proof covers at all, so its run falls back to the ladder that owns
//   ToInt32.
//
//   THE RECEIVERS. The gate HOISTS its loads above the stores between them,
//   which is legal because a read run's receiver was proven to be an Array and
//   a store run's to be a typed array — different objects. Where either proof
//   fails, nothing is hoisted because nothing is proven: an Array destination,
//   a non-Array source, a source with a getter, and a source that IS the
//   destination all take the slow arm and read back what the store before them
//   wrote.

function dump(a) {
  let s = '';
  for (let i = 0; i < a.length; i++) {
    if (i > 0) s += ',';
    s += a[i];
  }
  return s;
}

function seq(n) {
  const a = [];
  for (let i = 0; i < n; i++) a.push(i);
  return a;
}

// The span itself, spelled the way three.js spells it.
class M4 {
  constructor(e) {
    this.elements = e;
  }
  toArray(array = [], offset = 0) {
    const te = this.elements;
    array[offset] = te[0];
    array[offset + 1] = te[1];
    array[offset + 2] = te[2];
    array[offset + 3] = te[3];
    array[offset + 4] = te[4];
    array[offset + 5] = te[5];
    array[offset + 6] = te[6];
    array[offset + 7] = te[7];
    array[offset + 8] = te[8];
    array[offset + 9] = te[9];
    array[offset + 10] = te[10];
    array[offset + 11] = te[11];
    array[offset + 12] = te[12];
    array[offset + 13] = te[13];
    array[offset + 14] = te[14];
    array[offset + 15] = te[15];
    return array;
  }
}

function put(elements, target, offset) {
  return new M4(elements).toArray(target, offset);
}

// --- 1. Both dense and every element a Number: the arm the gate exists to
// reach. Sixteen loads, sixteen tests, one branch, sixteen stores.
console.log('dense: ' + dump(put(seq(16), new Float32Array(20), 2)));

// --- 2. One element a numeric STRING. It is not a Number, so the gate refuses
// and every member takes the ladder — which converts it, and gives the answer
// an all-numeric source would have given.
const withString = seq(16);
withString[3] = '3.5';
console.log('string: ' + dump(put(withString, new Float32Array(16), 0)));

// --- 3. One element an OBJECT whose valueOf counts and allocates. The gate
// converts nothing, so the count is the number of stores the ladder made of
// it: one. The allocation is what makes the members after it a use-after-move
// if anything on this path held a derived pointer across it.
let calls = 0;
let sink = null;
const counted = {
  valueOf: function () {
    calls++;
    sink = [];
    for (let i = 0; i < 300; i++) sink.push({ i: i, s: 'x' + i });
    return 55;
  }
};
const withObject = seq(16);
withObject[5] = counted;
console.log('valueof: ' + dump(put(withObject, new Float32Array(16), 0)) +
            ' calls=' + calls + ' sink=' + sink.length);

// --- 4. An explicit `undefined`. ToNumber(undefined) is NaN.
const withUndefined = seq(16);
withUndefined[7] = undefined;
console.log('undef: ' + dump(put(withUndefined, new Float32Array(16), 0)));

// --- 5. A HOLE. A dense element that is absent reads as `undefined`, which is
// the one answer the read arm's hole correction exists to give — and the gate
// tests bits that are above the Number range either way.
const withHole = seq(16);
delete withHole[9];
console.log('hole: ' + dump(put(withHole, new Float32Array(16), 0)));

// --- 6. `true`, `false` and `null`, which ToNumber sends to 1, 0 and 0.
const withOthers = seq(16);
withOthers[0] = true;
withOthers[2] = false;
withOthers[4] = null;
console.log('coerce: ' + dump(put(withOthers, new Float32Array(16), 0)));

// --- 7. A destination too SHORT for the run. One length test covers offset
// 15, so it fails and every member decides alone: ten land and six are
// discarded, with no named property created for any of them.
const short = put(seq(16), new Float32Array(10), 0);
console.log('short: ' + dump(short) + ' keys=' + Object.keys(short).length);

// --- 8. A destination long enough for the offset but not for the run: the
// offset pushes the last four members past the end.
console.log('straddle: ' + dump(put(seq(16), new Float32Array(20), 8)));

// --- 9. An ARRAY destination. Not a typed array, so the store proof refuses,
// and an Array element store performs no conversion at all — the string goes
// in as a string.
const arrayDst = seq(16);
for (let i = 0; i < 16; i++) arrayDst[i] = 0;
const withText = seq(16);
withText[3] = 'x';
console.log('arraydst: ' + dump(put(withText, arrayDst, 0)));

// --- 10. FLOAT64 against FLOAT32 on the same source: the wide view keeps 0.1
// and the narrow one keeps the nearest float to it.
const withTenth = seq(16);
withTenth[1] = 0.1;
console.log('f64: ' + dump(put(withTenth, new Float64Array(16), 0)));
console.log('f32: ' + dump(put(withTenth, new Float32Array(16), 0)));

// --- 11. An INT32 destination: a kind the store proof does not cover, so the
// run falls back and 7.1.6's ToInt32 applies to every member.
const halves = [];
for (let i = 0; i < 16; i++) halves.push((i % 2 === 0 ? -1 : 1) * (i + 0.5));
console.log('int32: ' + dump(put(halves, new Int32Array(16), 0)));

// --- 12. A source that is not an Array. The read proof refuses, so the whole
// span takes the slow arm even though the destination is perfectly provable.
const objectSource = {};
for (let i = 0; i < 16; i++) objectSource[i] = 100 + i;
console.log('object: ' + dump(put(objectSource, new Float32Array(16), 0)));

// --- 13. A source whose element 5 is a GETTER that writes into the
// destination mid-run. Nothing is hoisted over it, because nothing about this
// source was proven: index 0 is written with 0, the getter overwrites it with
// 99, and the rest of the run follows. (The source is a plain object rather
// than an array with an accessor element, which bronze refuses to describe.)
let getterCalls = 0;
const target13 = new Float32Array(16);
const getterSource = {
  get '5'() {
    getterCalls++;
    target13[0] = 99;
    return 5;
  }
};
for (let i = 0; i < 16; i++) {
  if (i !== 5) getterSource[i] = i;
}
console.log('getter: ' + dump(put(getterSource, target13, 0)) + ' calls=' + getterCalls);

// --- 14. `m.toArray(m.elements, 1)`: the source IS the destination, and it is
// an Array. Every read sees what the store before it wrote, so element i+1
// takes element i and the whole thing ends as element zero — sixteen times,
// with index 16 created on the way.
const selfArray = new M4(seq(16));
console.log('selfarray: ' + dump(selfArray.toArray(selfArray.elements, 1)));

// --- 15. The same with a typed array on BOTH sides. The read proof wants an
// Array and gets a view, so it refuses and the span bleeds exactly as above —
// except that the sixteenth store is out of range and discarded.
const selfView = new Float32Array(16);
for (let i = 0; i < 16; i++) selfView[i] = i;
const selfTyped = new M4(selfView);
console.log('selftyped: ' + dump(selfTyped.toArray(selfView, 1)));

// --- 16. The default arms of the same method: no destination and no offset,
// so the run writes into a fresh Array.
console.log('defaults: ' + dump(new M4(seq(16)).toArray()));

// --- 17. The whole thing in a loop, which is what `InstancedMesh.setMatrixAt`
// is: one matrix written into eight slices of one view, with the twelfth
// element changing between them. The sum is what a missed store or a doubled
// one would move.
const big = new Float32Array(16 * 8);
const churn = new M4(seq(16));
for (let i = 0; i < 8; i++) {
  churn.elements[12] = i;
  churn.toArray(big, i * 16);
}
let total = 0;
for (let i = 0; i < big.length; i++) total += big[i];
console.log('churn: ' + total + ' ' + big[12] + ' ' + big[28] + ' ' + big[124]);

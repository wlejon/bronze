// The STORE-RUN RECEIVER PROOF (src/codegen-llvm/llvm_store_proof.h): a run of
// `target[base + k] = v` writes that proves the receiver, the element kind and
// the bound once and then spends that proof on each member.
//
// Everything below is a question about what the language says, not about what
// the proof does — the proof is only sound if turning it off changes nothing.
// The spec sections each group pins:
//
//   10.4.5.5  [[Set]] on a typed array: a canonical numeric index string goes
//             to TypedArraySetElement, which returns true either way, so an
//             invalid index is a DISCARDED write and never a throw and never a
//             named property.
//   10.4.5.16 TypedArraySetElement: ToNumber(value) runs BEFORE
//             IsValidIntegerIndex(O, index) is asked, so a value whose
//             conversion throws throws even at an index that would have been
//             discarded.
//   10.4.5.14 IsValidIntegerIndex: false for a detached view, for a
//             non-integral index, for -0F, and for an index outside
//             [0, length).
//   7.1.4     ToNumber: a numeric string converts, an object goes through
//             ToPrimitive and so can run user code that allocates or throws.
//   6.1.7     Array index / CanonicalNumericIndexString: `ta[-0]` is
//             `ta["0"]`, because ToString(-0F) is "0". The -0F that
//             IsValidIntegerIndex refuses can only come from the literal
//             string key "-0".

function dump(a) {
  let s = '';
  for (let i = 0; i < a.length; i++) {
    if (i > 0) s += ',';
    s += a[i];
  }
  return s;
}

// The run the proof exists for, four wide: one receiver, one base, offsets
// 0..3 known at compile time.
function put4(target, offset, v0, v1, v2, v3) {
  target[offset] = v0;
  target[offset + 1] = v1;
  target[offset + 2] = v2;
  target[offset + 3] = v3;
  return target;
}

function fresh(n) {
  return new Float32Array(n);
}

// --- 1. A Float32Array run with an ordinary numeric offset. ----------------
console.log('plain: ' + dump(put4(fresh(8), 2, 1, 2, 3, 4)));

// --- 2. -0 as the offset. 6.1.7: ToString(-0F) is "0", so `ta[-0]` is
// `ta["0"]`, whose canonical numeric index is +0F and therefore valid. The
// three offsets after it are -0 + 1 == 1 and so on.
console.log('negzero: ' + dump(put4(fresh(8), -0, 5, 6, 7, 8)));

// --- 3. A fractional offset. 10.4.5.14: not an integral number, so every
// member of the run is discarded — no throw, and no named property either.
const frac = put4(fresh(4), 1.5, 1, 2, 3, 4);
console.log('fraction: ' + dump(frac) + ' keys=' + Object.keys(frac).length);

// --- 4. A negative offset. Index -1 is refused; 0, 1 and 2 are not. The run
// straddles the bottom of the view, so each member decides for itself.
console.log('negative: ' + dump(put4(fresh(6), -1, 1, 2, 3, 4)));

// --- 5. A NaN offset. CanonicalNumericIndexString("NaN") is NaN, which is a
// canonical numeric index and not an integral number: all four discarded.
const nan = put4(fresh(4), NaN, 1, 2, 3, 4);
console.log('nan: ' + dump(nan) + ' keys=' + Object.keys(nan).length);

// --- 6. An offset past the end. All four outside [0, length): all discarded.
const over = put4(fresh(4), 100, 1, 2, 3, 4);
console.log('overlength: ' + dump(over) + ' keys=' + Object.keys(over).length);

// --- 7. A run that straddles the TOP of the view: indices 6 and 7 land, 8 and
// 9 are discarded. The one bound the proof would take covers the largest
// offset, so a straddling run cannot be proven and each store decides alone.
console.log('straddle: ' + dump(put4(fresh(8), 6, 1, 2, 3, 4)));

// --- 8. Float32 narrowing against Float64. The same run into the two kinds:
// the f32 view rounds 0.1 to the nearest float, and reading it back widens
// that float and prints its exact value.
const f32 = put4(new Float32Array(4), 0, 0.1, 0.5, 1e40, -1e40);
const f64 = put4(new Float64Array(4), 0, 0.1, 0.5, 1e40, -1e40);
console.log('f32: ' + dump(f32));
console.log('f64: ' + dump(f64));

// --- 9. A value that is a numeric string. 7.1.4 converts it; the proof's fast
// arm only takes values that are already Numbers, so this member takes the
// ladder that owns the conversion.
console.log('numstr: ' + dump(put4(fresh(4), 0, '7', ' 8 ', '0x10', 'nope')));

// --- 10. A value whose valueOf ALLOCATES. The conversion can collect, so the
// proof's derived pointer cannot survive this member — and the members after
// it must still store the right bytes.
let sink = null;
const allocating = {
  valueOf: function () {
    sink = [];
    for (let i = 0; i < 300; i++) sink.push({ i: i, s: 'x' + i });
    return 5;
  }
};
console.log('alloc: ' + dump(put4(fresh(4), 0, 1, allocating, 3, 4)) + ' sink=' + sink.length);

// --- 11. A value whose valueOf THROWS, mid-run. Everything stored before it
// is visible after the catch; nothing after it ran.
const boom = {
  valueOf: function () {
    throw new Error('boom');
  }
};
const partial = fresh(4);
let caught = '';
try {
  put4(partial, 0, 1, 2, boom, 4);
} catch (e) {
  caught = e.message;
}
console.log('throwmid: ' + dump(partial) + ' caught=' + caught);

// --- 12. 10.4.5.16's order: the conversion runs BEFORE the index is checked,
// so a throwing value throws even at an index that would have been discarded.
const oob = fresh(4);
let caughtOob = '';
try {
  oob[99] = boom;
} catch (e) {
  caughtOob = e.message;
}
console.log('oobthrow: ' + dump(oob) + ' caught=' + caughtOob);

// --- 13. An Array receiver. Not a typed array at all, so the proof refuses
// the whole run and every member takes the path it always took.
console.log('array: ' + dump(put4([0, 0, 0, 0, 0, 0, 0, 0], 2, 1, 2, 3, 4)));

// --- 14. A plain-object receiver. The indices become ordinary named
// properties, in ascending integer-key order.
const plain = put4({}, 3, 'a', 'b', 'c', 'd');
console.log('plain-object: ' + Object.keys(plain).join('|') + ' ' + plain[3] + plain[4] +
            plain[5] + plain[6]);

// --- 15. An Int32Array receiver: a kind the proof does not cover, so the run
// falls back and 7.1.6's ToInt32 still applies to every member.
console.log('int32: ' + dump(put4(new Int32Array(6), 1, 3.9, -1.5, 2147483648, NaN)));

// --- 16. The source and the target being the SAME object. The reads off it
// and the writes into it interleave over one receiver, and the copy is the
// identity.
function copy4(dst, off, src) {
  dst[off] = src[0];
  dst[off + 1] = src[1];
  dst[off + 2] = src[2];
  dst[off + 3] = src[3];
  return dst;
}
const selfArr = [10, 11, 12, 13];
console.log('self: ' + dump(copy4(selfArr, 0, selfArr)));

// --- 17. A source whose element 1 is a GETTER that writes into the target
// mid-run. A read whose fast arm is not taken reaches bronze_prop_get, which
// runs the getter, which can collect — so the store proof cannot survive that
// member, and what is pinned here is the observable order: index 0 is written
// with 10, the getter then overwrites it with 99, and the rest of the run
// follows. (The source is a plain object rather than an array with an accessor
// element, which bronze refuses to describe.)
const target17 = fresh(4);
const src17 = {
  '0': 10,
  get '1'() {
    target17[0] = 99;
    return 11;
  },
  '2': 12,
  '3': 13
};
console.log('getter: ' + dump(copy4(target17, 0, src17)));

// --- 18. The shape all of this exists for: three.js's Matrix4.toArray, called
// the way InstancedMesh.setMatrixAt calls it — sixteen reads off one array and
// sixteen affine writes into a Float32Array, interleaved.
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

class IM {
  constructor(n) {
    this.instanceMatrix = { array: new Float32Array(n * 16) };
  }
  setMatrixAt(index, matrix) {
    matrix.toArray(this.instanceMatrix.array, index * 16);
  }
}

const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const im = new IM(3);
const m4 = new M4(identity.slice());
for (let i = 0; i < 3; i++) {
  m4.elements[12] = i * 2;
  m4.elements[13] = i * 2 + 0.5;
  im.setMatrixAt(i, m4);
}
console.log('toArray: ' + dump(im.instanceMatrix.array));

// The default-argument arms of the same method: no target and no offset, so
// the run writes into a fresh Array rather than a view.
console.log('defaults: ' + dump(m4.toArray()));

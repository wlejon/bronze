// The HARD half of the out-of-bounds story. typed_array_detach.js pins the
// soft surface — element access answers undefined/discard, the length family
// answers 0 — and this case pins everything that THROWS instead: prototype
// methods and iteration over a closed window (23.2.3's ValidateTypedArray,
// 23.1.5.1's next), every DataView access over one (25.3.1.1-.2 step 6, the
// 25.3.4.2-.3 getters, the 25.3.2.1 constructor), and the one store that owes
// a conversion even when the window is closed (ToBigInt of a Number).
function kind(fn) {
  try {
    fn();
    return "no throw";
  } catch (e) {
    if (e instanceof TypeError) return "TypeError";
    if (e instanceof RangeError) return "RangeError";
    return "other";
  }
}

// A view a shrinking resize stranded — closed but NOT detached — is a
// TypeError from every method, and a regrow reopens it (the regrown bytes
// zero-filled per 25.1.5.5).
const rbuf = new ArrayBuffer(32, { maxByteLength: 32 });
const r = new Float64Array(rbuf, 16, 2);
r[0] = 2.5;
r[1] = 3.5;
console.log(r.join(","));
rbuf.resize(8);
console.log(kind(() => r.join(",")));
console.log(kind(() => r.fill(1)));
console.log(kind(() => r.at(0)));
rbuf.resize(32);
console.log(r.join(","));

// for-of: the iterator's next() asks out-of-bounds BEFORE the length, so a
// transfer MID-LOOP surfaces as a TypeError at the next step, not a quiet
// early end — and a loop opened on an already-detached view throws before
// yielding anything.
const buf = new ArrayBuffer(32);
const a = new Float64Array(buf);
a[0] = 1;
a[1] = 2;
a[2] = 3;
a[3] = 4;
let seen = 0;
console.log(kind(() => {
  for (const x of a) {
    seen++;
    if (seen === 2) buf.transfer();
  }
}));
console.log(seen);
console.log(kind(() => { for (const x of a) {} }));

// The explicit iterator agrees — and a closed window REOPENS for it too: the
// failed next() does not advance, and after the regrow the same iterator
// resumes exactly where it stood.
const rbuf2 = new ArrayBuffer(16, { maxByteLength: 16 });
const v2 = new Float64Array(rbuf2);
v2[0] = 5;
const it = v2.values();
console.log(it.next().value);
rbuf2.resize(0);
console.log(kind(() => it.next()));
rbuf2.resize(16);
console.log(it.next().value, it.next().done);

// DataView: every access is a TypeError over a closed window — deliberately
// unlike a typed array's soft element answers — and the byteLength/byteOffset
// getters throw where the typed-array ones answer 0. A merely-too-far index
// on a HEALTHY buffer stays the RangeError it always was.
const dbuf = new ArrayBuffer(8);
const dv = new DataView(dbuf);
dv.setFloat64(0, 3.25);
console.log(dv.getFloat64(0), dv.byteLength, dv.byteOffset);
console.log(kind(() => dv.getFloat64(1)));
const dmoved = dbuf.transfer();
console.log(kind(() => dv.getFloat64(0)));
console.log(kind(() => dv.setUint8(0, 1)));
console.log(kind(() => dv.byteLength));
console.log(kind(() => dv.byteOffset));
console.log(dv.buffer === dbuf, dbuf.detached);
console.log(kind(() => new DataView(dbuf)));
console.log(new DataView(dmoved).getFloat64(0));

// 25.3.1.2 converts the VALUE first (step 4) and asks validity after (step
// 6): a valueOf that transfers the buffer away has run — the flag proves it —
// and the store it sabotaged is the TypeError, not a write into dead bytes.
const dbuf2 = new ArrayBuffer(8);
const dv2 = new DataView(dbuf2);
let converted = false;
console.log(kind(() => dv2.setFloat64(0, {
  valueOf() {
    converted = true;
    dbuf2.transfer();
    return 7;
  },
})));
console.log(converted);

// A DataView over a shrinking resize closes and reopens on the typed-array
// view's terms, but with TypeErrors while closed.
const dr = new ArrayBuffer(16, { maxByteLength: 16 });
const dvr = new DataView(dr, 8, 8);
dvr.setUint8(0, 42);
console.log(dvr.getUint8(0));
dr.resize(4);
console.log(kind(() => dvr.getUint8(0)), kind(() => dvr.byteLength));
dr.resize(16);
console.log(dvr.getUint8(0), dvr.byteLength);

// The BigInt store's debt survives the closing: 10.4.5.16 converts BEFORE it
// re-checks the index, and ToBigInt has no Number row — so a Number store
// into a closed BigInt64Array window still throws, on every path, while a
// BigInt store discards quietly and a read answers undefined.
const bbuf = new ArrayBuffer(16);
const big = new BigInt64Array(bbuf);
big[0] = 5n;
console.log(String(big[0]));
bbuf.transfer();
console.log(kind(() => { big[0] = 1; }));
big[0] = 7n;
console.log(big[0], big.length);

// Detached IS out of bounds even when the arithmetic passes: a zero-length
// window at offset 0 has nothing to overhang, and throws anyway.
const zbuf = new ArrayBuffer(8);
const z = new Float64Array(zbuf, 0, 0);
const zdv = new DataView(zbuf, 0, 0);
zbuf.transfer();
console.log(z.length, kind(() => z.join(",")));
console.log(kind(() => zdv.byteLength));

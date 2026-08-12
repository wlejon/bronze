// `DataView`'s two error ladders (ECMA-262 25.3.2.1 for the constructor,
// 25.3.1.1 / 25.3.1.2 for an access), and the coercion that runs before each of
// them.
//
// The distinction the whole case is about is TypeError versus RangeError, and
// it is not a stylistic one — the two rungs are different STEPS:
//
//  - 25.3.2.1 step 2 is RequireInternalSlot(buffer, [[ArrayBufferData]]). A
//    value that is not an ArrayBuffer is the wrong KIND of thing, so it is a
//    TypeError. A Uint8Array is on this rung and not off the ladder: it has
//    [[ViewedArrayBuffer]], not [[ArrayBufferData]], so `new DataView(u8)`
//    throws where `new DataView(u8.buffer)` succeeds.
//  - Every other rung — steps 3, 6 and 9, and step 3 of both accessor
//    operations — is about a NUMBER that is out of range, and every one of them
//    is a RangeError. Two of them come from 7.1.22 ToIndex (a negative or
//    non-finite argument) and the rest from a comparison against the buffer's
//    or the view's length.
//
// ToIndex is also what makes a FRACTION legal: it is
// ToIntegerOrInfinity, which truncates towards zero, so `getUint8(1.9)` reads
// byte 1 and `new DataView(buffer, -0.5)` is offset 0 — ℝ(-0) is 0, which is in
// ToIndex's range, so the minus sign is not by itself an error. `NaN` is +0 by
// the same step, and an infinity is the RangeError.
//
// What is NOT pinned here, because bronze cannot yet observe it: 25.3.1.2 runs
// ToNumber on the VALUE (step 4) before the bounds test (step 11), so
// `setUint8(999, x)` converts x and then throws. Observing the order needs a
// conversion with a side effect — a `valueOf`, which is ToPrimitive, or a
// Symbol, whose ToNumber is a hard error in bronze rather than a catchable
// TypeError. The order is implemented; the witness for it is not writable yet.

const buffer = new ArrayBuffer(8);

// The name of what a thunk throws, or what it returned. `e.name` rather than
// the message, because the message is bronze's wording and the CLASS is
// ECMA-262's.
function thrown(fn) {
  try {
    return "ok " + fn();
  } catch (e) {
    return e.name;
  }
}

// --- 25.3.2.1 step 2: the wrong kind of first argument is a TypeError.
console.log(thrown(() => new DataView(new Uint8Array(8)).byteLength));
console.log(thrown(() => new DataView({}).byteLength));
console.log(thrown(() => new DataView("buffer").byteLength));
console.log(thrown(() => new DataView(8).byteLength));
console.log(thrown(() => new DataView().byteLength));
// ...and the same value reached through `.buffer` is the right kind.
console.log(thrown(() => new DataView(new Uint8Array(8).buffer).byteLength));

// --- 25.3.2.1 step 3: ToIndex on byteOffset. A RangeError, and a truncation.
console.log(thrown(() => new DataView(buffer, -1).byteOffset));
console.log(thrown(() => new DataView(buffer, Infinity).byteOffset));
console.log(thrown(() => new DataView(buffer, -Infinity).byteOffset));
console.log(thrown(() => new DataView(buffer, -0.5).byteOffset));
console.log(thrown(() => new DataView(buffer, 1.9).byteOffset));
console.log(thrown(() => new DataView(buffer, NaN).byteOffset));

// --- 25.3.2.1 step 6: an offset past the end. Offset == length is legal and
// makes an empty view; one more is the RangeError.
console.log(thrown(() => new DataView(buffer, 8).byteLength));
console.log(thrown(() => new DataView(buffer, 9).byteLength));

// --- 25.3.2.1 steps 8 and 9: the length. Omitted spans the rest of the buffer,
// and there is no divisibility rule to fail — a DataView has no element width,
// which is where a Float32Array over the same remainder would differ.
console.log(thrown(() => new DataView(buffer, 5).byteLength));
console.log(thrown(() => new DataView(buffer, 4, 4).byteLength));
console.log(thrown(() => new DataView(buffer, 4, 5).byteLength));
console.log(thrown(() => new DataView(buffer, 0, 9).byteLength));
console.log(thrown(() => new DataView(buffer, 0, -1).byteLength));
console.log(thrown(() => new DataView(buffer, 8, 0).byteLength));

// --- 25.3.1.1 step 9 / 25.3.1.2 step 11: the access bounds, which depend on
// the WIDTH the accessor names and not on the view alone. The same index is in
// range for one accessor and past the end for a wider one.
const view = new DataView(buffer);
console.log(thrown(() => view.getUint8(7)));
console.log(thrown(() => view.getUint8(8)));
console.log(thrown(() => view.getUint16(6)));
console.log(thrown(() => view.getUint16(7)));
console.log(thrown(() => view.getUint32(4)));
console.log(thrown(() => view.getUint32(5)));
console.log(thrown(() => view.getFloat64(0)));
console.log(thrown(() => view.getFloat64(1)));
console.log(thrown(() => view.setUint8(8, 1)));
console.log(thrown(() => view.setFloat64(1, 1)));

// --- 25.3.1.1 step 3: ToIndex on the access index, the same rung as the
// constructor's and so the same class of error.
console.log(thrown(() => view.getUint8(-1)));
console.log(thrown(() => view.getUint8(Infinity)));
console.log(thrown(() => view.getUint8()));
view.setUint8(1, 42);
console.log(thrown(() => view.getUint8(1.9)));
console.log(thrown(() => view.getUint8(NaN)));

// --- A WINDOWED view is bounded by its own byteLength and not the buffer's,
// which is the whole difference between the two slots.
const windowed = new DataView(buffer, 4, 2);
console.log(thrown(() => windowed.getUint16(0)));
console.log(thrown(() => windowed.getUint16(1)));
console.log(thrown(() => windowed.getUint32(0)));

// --- 25.3.1.1 step 1: RequireInternalSlot([[DataView]]) on the RECEIVER, so a
// method taken off the object and called on something else names what it is
// rather than reading whatever `this` happened to be. A TypeError, because it
// is again the wrong kind of thing.
const loose = view.getUint8;
console.log(thrown(() => loose(0)));
console.log(thrown(() => loose.call(buffer, 0)));
console.log(thrown(() => loose.call(view, 1)));

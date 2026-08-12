// Two views over ONE buffer, each seeing what the other wrote — which is the
// whole reason `new DataView(buffer)` takes a buffer rather than a length.
// 25.3.2.1 stores the argument in [[ViewedArrayBuffer]]; it does not copy it,
// so `view.buffer === buffer` is identity and every write through either side
// is a write to the same bytes.
//
// The typed-array partner here is a `Uint8Array` and an `Int8Array`, and that
// is deliberate. A one-byte element has no byte order to have, so what these
// pin is a fact about the LANGUAGE. A Uint32Array element would be read with
// the byte order of the surrounding agent (6.1.6.1 / the [[LittleEndian]] field
// of the Agent Record), which is a fact about the machine — the exact thing a
// DataView exists so a program never has to depend on, and so the exact thing
// an oracle expectation must not pin.
//
// The rest of the case is the object itself: its slots, the identity of the
// constructor and of the accessors, what `in` finds, and the inspect form.
// `getBigInt64` is IN the object — 25.3.4 defines it — even though bronze has
// no BigInt to give it; the name existing and the value being unavailable are
// two different answers, and only the second is bronze's limitation.

const shared = new ArrayBuffer(8);
const bytes = new Uint8Array(shared);
const full = new DataView(shared);
const tail = new DataView(shared, 4);

console.log(full.byteLength, full.byteOffset, tail.byteLength, tail.byteOffset);
console.log(full.buffer === shared, tail.buffer === shared, full.buffer === tail.buffer);

// --- Written through the DataView, read through the typed array.
full.setUint32(0, 16909060);
console.log(bytes[0], bytes[1], bytes[2], bytes[3]);

// --- Written through the typed array, read through both DataViews. The
// windowed one addresses the same bytes from its own origin, so index 0 of the
// window is index 4 of the buffer.
bytes[4] = 170;
bytes[5] = 187;
bytes[6] = 204;
bytes[7] = 221;
console.log(tail.getUint32(0), full.getUint32(4));
console.log(tail.getUint32(0, true), full.getUint32(4, true));

// --- Back the other way, through the window.
tail.setUint16(0, 258);
console.log(bytes[4], bytes[5], bytes[6], bytes[7]);
console.log(full.getUint16(4));

// --- The same byte, signed and unsigned, through three objects at once.
const signed = new Int8Array(shared);
full.setInt8(0, -1);
console.log(bytes[0], signed[0], full.getInt8(0), full.getUint8(0));
signed[0] = -2;
console.log(bytes[0], full.getInt8(0), full.getUint8(0));

// --- A DataView built over a typed array's WINDOW, which is the idiom for
// reinterpreting part of a buffer: the three slots a view exposes are exactly
// the three arguments the constructor takes.
const window8 = new Uint8Array(shared, 4, 4);
const overWindow = new DataView(window8.buffer, window8.byteOffset, window8.byteLength);
console.log(overWindow.byteOffset, overWindow.byteLength);
overWindow.setUint16(0, 4660);
console.log(window8[0], window8[1], bytes[4], bytes[5]);

// --- The object's identity and members.
console.log(full.constructor === DataView, typeof full, typeof DataView);
console.log("byteLength" in full, "byteOffset" in full, "buffer" in full);
console.log("getUint16" in full, "setFloat64" in full, "getBigInt64" in full, "nope" in full);
// 25.3 gives a DataView no indexed access at all: every member above lives on
// the prototype, and `view[0]` is a name the object simply does not have. It is
// `undefined` and not an error, because that is what a missing property is —
// the contrast with the RangeError `getUint8(0)` raises past the end is the
// point, since one is a property that does not exist and the other a byte.
console.log(full[0], full[1], full.length);
// One function object per accessor, shared by every DataView — which is what
// `DataView.prototype.getUint16` being a single function means — and a
// different one per width.
console.log(full.getUint16 === full.getUint16, full.getUint16 === tail.getUint16);
console.log(full.getUint16 === full.getUint32, full.getUint16 === full.setUint16);

// --- The inspect form. A buffer prints as its bytes, because that is the whole
// of what it is; a DataView prints its three slots with the buffer inside them,
// and the buffer is one level deeper than the view for the depth cut.
const small = new ArrayBuffer(4);
const sv = new DataView(small);
sv.setUint16(0, 258);
console.log(small);
console.log(sv);
console.log(new DataView(small, 2, 2));
console.log(new ArrayBuffer(0));
console.log([sv]);
console.log([[sv]]);
console.log([[[sv]]]);

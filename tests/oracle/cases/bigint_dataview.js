// 25.3.4's four 64-bit accessors, the only DataView members whose value is a
// BigInt. They are the reason a 64-bit integer can round-trip through bytes at
// all: no Number holds 2^63 - 1, so getFloat64 cannot stand in for them.
const buf = new ArrayBuffer(24);
const view = new DataView(buf);

// Big-endian is the default, so a value written without the flag reads back
// byte-reversed with it.
view.setBigInt64(0, -1n);
console.log(view.getBigInt64(0), view.getBigUint64(0));
view.setBigUint64(0, 0n);
console.log(view.getBigInt64(0), view.getBigUint64(0));

const edges = [0n, 1n, -1n, 2n ** 63n - 1n, -(2n ** 63n), 2n ** 64n - 1n,
               9007199254740993n, -9007199254740993n];
for (const v of edges) {
  view.setBigUint64(8, BigInt.asUintN(64, v));
  const signed = view.getBigInt64(8);
  const unsigned = view.getBigUint64(8);
  console.log(v, signed, unsigned, BigInt.asIntN(64, v) === signed);
}

// The byte order flag, checked against the individual bytes.
view.setBigUint64(16, 0x0102030405060708n);
console.log(view.getUint8(16), view.getUint8(23), view.getBigUint64(16, true).toString(16));
view.setBigUint64(16, 0x0102030405060708n, true);
console.log(view.getUint8(16), view.getUint8(23), view.getBigUint64(16).toString(16));

// 25.3.1.5 NumericToRawBytes for the BigInt rows is modulo 2^64, so a value
// too wide WRAPS the way setInt32 wraps rather than throwing.
view.setBigInt64(0, 2n ** 64n + 5n);
console.log(view.getBigUint64(0));

function fail(f) {
  try { return String(f()); } catch (e) {
    const kind = e instanceof RangeError ? "RangeError" : e instanceof TypeError ? "TypeError" : "Error";
    return kind + ": " + e.message;
  }
}
// Step 4 is ToBigInt, and 7.1.13 refuses a Number outright — there is no
// implicit widening of 1 to 1n.
console.log(fail(() => view.setBigInt64(0, 1)));
console.log(fail(() => view.setBigUint64(0, 1.5)));
console.log(fail(() => view.getBigInt64(17)));
console.log(fail(() => view.setBigInt64(17, 0n)));
console.log(fail(() => view.getBigUint64(-1)));
console.log(view.setBigInt64(0, 7n), view.getBigInt64(0));

// They are ordinary members of DataView.prototype, found by the ordinary walk.
console.log("getBigInt64" in view, typeof view.setBigUint64);

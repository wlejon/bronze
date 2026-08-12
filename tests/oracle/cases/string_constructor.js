// The global `String` as a CONVERSION function and as a value (docs/0030
// decision 4). `String.prototype` as a real object a program can hold is a
// separate value-model chunk (cases/blocked/object_intrinsic_prototypes), and
// nothing here depends on one.
//
// From ECMA-262:
//
// 1. 22.1.1.1 (String) step 1: called with NO argument the result is the empty
//    string, which is not the same as `String(undefined)` — that one is
//    ToString(undefined), the six characters "undefined".
// 2. ToString of a number is 6.1.6.1.20 Number::toString: -0 is "0" (the sign
//    is not carried, unlike console.log's inspect spelling, docs/0013), 1e21 is
//    "1e+21" because 21 is where the fixed notation stops, and 0.1 + 0.2 is the
//    seventeen-digit "0.30000000000000004" that shortest-round-trip requires.
// 3. 22.1.2.1 (String.fromCharCode): each argument is ToUint16 (7.1.7), which
//    truncates towards zero and then takes it modulo 2^16 — so 65.9 is "A",
//    65601 is "A" again, -1 is the same unit as 65535, and NaN is U+0000. With
//    no arguments the result is the empty string.
// 4. A code unit above U+00FF is a real character and not an escape: 0x20AC is
//    the euro sign, and it is one character of `length` 1.
// 5. 10.2.5 / 22.1.3.2: `String.prototype.constructor` is `String`, so
//    `"".constructor === String` and it is the same object the bare name reads.

console.log(String(5), String(-0), String(true), String(null), String(undefined));
console.log(String() === "", String().length, String("already"));
console.log(String(NaN), String(Infinity), String(-Infinity));
console.log(String(1e21), String(0.1 + 0.2), String(-7.5));

console.log(String.fromCharCode(65, 66, 67));
console.log(String.fromCharCode() === "", String.fromCharCode().length);
console.log(String.fromCharCode(65.9), String.fromCharCode(65601));
console.log(String.fromCharCode(-1) === String.fromCharCode(65535));
console.log(String.fromCharCode(NaN) === String.fromCharCode(0));
console.log(String.fromCharCode(0x20AC), String.fromCharCode(0x20AC).length);

console.log("".constructor === String, "abc".constructor === String);
console.log(String(5).constructor === String, typeof String, typeof String(5));

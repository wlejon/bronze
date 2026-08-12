// What a STORE into each element kind does with a value that does not fit,
// which is ECMA-262 10.4.5.5 IntegerIndexedElementSet -> 7.1.6..7.1.11.
//
// For the integer kinds (7.1.6 ToInt8 and its siblings): a non-finite value
// and both zeroes become +0; everything else truncates towards zero and is
// then taken modulo 2^N, with the signed kinds re-signed above 2^(N-1). Note
// what that makes of 1e40: the double is an exact integer whose factorisation
// contains 2^80, so every modulus here divides it and the answer is 0 for all
// six — not "some large number", and not the saturation a C cast would give.
//
// For Uint8Clamped (7.1.11 ToUint8Clamp) none of that applies: it saturates
// at both ends and rounds .5 to EVEN, the one place JS does not round half
// away from zero.
//
// The constructors are used as VALUES here, which they are, so the table below
// is a loop rather than seven copies.
const integerKinds = [
  Int8Array, Uint8Array, Uint8ClampedArray,
  Int16Array, Uint16Array, Int32Array, Uint32Array,
];
const inputs = [-1, 256, 0 / 0, 1e40, 0.5, -0.5];

for (const K of integerKinds) {
  const v = new K(inputs.length);
  for (let i = 0; i < inputs.length; i++) v[i] = inputs[i];
  console.log(v);
}

// The float kinds narrow instead of wrapping: Float32 rounds to the nearest
// representable single and overflows to Infinity, Float64 stores what it was
// given. A stored -0 keeps its sign, and inspect reports it.
const f32 = new Float32Array(inputs.length);
const f64 = new Float64Array(inputs.length);
for (let i = 0; i < inputs.length; i++) {
  f32[i] = inputs[i];
  f64[i] = inputs[i];
}
console.log(f32);
console.log(f64);

// Round half to even, across the clamp at both ends.
const halves = [0.5, 1.5, 2.5, 3.5, 4.5, 253.5, 254.5, 255.5];
const clamped = new Uint8ClampedArray(halves.length);
for (let i = 0; i < halves.length; i++) clamped[i] = halves[i];
console.log(clamped);

// ...and that it is round-half-to-even and not round-down: anything past the
// midpoint still goes up.
const nearHalves = [2.4, 2.6, 3.4, 3.6];
const nearest = new Uint8ClampedArray(nearHalves.length);
for (let i = 0; i < nearHalves.length; i++) nearest[i] = nearHalves[i];
console.log(nearest);

// The sign of a truncated-to-zero store is +0, because 7.1.6 step 3 truncates
// the MATHEMATICAL value: `-0.5` into an Int8Array is +0, and printing it as
// `-0` would announce a sign the conversion does not produce.
const signs = new Int8Array(2);
signs[0] = -0.5;
signs[1] = -0;
console.log(signs);
const keepsSign = new Float64Array(1);
keepsSign[0] = -0;
console.log(keepsSign);

// 10.4.5.4/10.4.5.5: an index outside the length is not a property of a typed
// array, so the write is DISCARDED and the read is `undefined` — it does not
// grow, and it does not become a named property either.
const short = new Int8Array(2);
short[5] = 7;
console.log(short[5], short.length, short);

// 7.1.22 ToIndex truncates rather than rejecting, so a fractional length is a
// shorter array and not an error; a negative one is the RangeError of step
// 2.c.
console.log(new Float32Array(1.5).length, new Float32Array(0 / 0).length);
try {
  new Float32Array(-1);
} catch (e) {
  console.log(e.name);
}

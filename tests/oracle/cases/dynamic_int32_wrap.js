// ECMA-262 7.1.6 ToInt32 and 7.1.7 ToUint32 through a DYNAMIC operand — the
// path where the value's type is unknown at compile time and the backend
// inlines a number test in front of the conversion.
//
// Every value here is outside the int32 range, which is the case the inline
// path used to get wrong: `fptosi` of a double the destination cannot hold is
// POISON in LLVM, not a wrong number, so the answer was whatever the optimizer
// made of a value it was told could not occur. ToInt32 does not saturate — it
// truncates toward zero and reduces MODULO 2^32 — so every expectation below
// is the mathematical residue and never a clamp to 2147483647.
const box = { v: 0 };
const values = [
  4294967296, 4294967297, -4294967296, -4294967297,
  2147483647, 2147483648, 2147483649, -2147483648, -2147483649,
  4294967295.75, -4294967295.75, 3000000000, 1e10, 1e21, 1e100,
  NaN, Infinity, -Infinity, 0, -0, 0.5, -0.5,
];
for (const v of values) {
  box.v = v;
  console.log(box.v | 0, box.v >>> 0, ~box.v, box.v & -1, box.v << 0, box.v >> 0);
}

// The same conversion reached through ToNumber first, which is what makes the
// operand genuinely unknown: a string, a boolean and an object with `valueOf`.
function coerce(x) { return x | 0; }
console.log(coerce("4294967297"), coerce("0x100000001"), coerce(true), coerce(null));
console.log(coerce({ valueOf() { return 2147483648; } }), coerce([1e10]), coerce("nope"));

// And the shift-count mask, which is ToUint32 of the right operand and then
// its low five bits — so a huge count is not a huge shift.
console.log(1 << 32, 1 << 33, 1 << 4294967296, 1 << -1, box.v);

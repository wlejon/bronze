// The evaluation ORDER of a `+` chain, and what may and may not be observed
// about the accumulator that builds one.
//
// A left-associative spine of three or more `+` lowers to an accumulator —
// `concat.begin`, one `concat.append` per further operand, `concat.end` — with
// a String result carried between the steps as a builder: one allocation with
// room to grow instead of one flat copy per operator. Everything below is a
// question that a WRONG version of that transformation answers differently
// from a right one, and the right answers all come from ECMA-262 13.15.3
// ApplyStringOrNumericBinaryOperator.
//
// 13.15.3 for `((a + b) + c) + d` fixes the order as: evaluate a, evaluate b,
// ToPrimitive(a), ToPrimitive(b), decide String-or-numeric and convert, THEN
// evaluate c, ToPrimitive of the accumulator, ToPrimitive(c), and so on. Every
// one of those steps can run program text, so the order is observable and an
// N-ary "evaluate everything, then convert everything" helper would be a
// different language:
//
//  1. An operand's `toString` runs BEFORE a later operand is evaluated, so a
//     later operand that mutates what the earlier `toString` reads cannot
//     affect it. The trace pins both readings of the same object.
//  2. The hint is DEFAULT, not string: 7.1.1 asks `valueOf` first, and the
//     same object stringifies differently under `String(...)` and `${...}`,
//     which ask hint String.
//  3. A spine can start NUMERIC and become a String part way down — 13.15.3
//     decides per operator, not per spine — and both halves must be right.
//     (A spine this lowering takes always ENDS as a String, because it is
//     offered only where an operand is certainly one; the numeric half is
//     what `concat.begin` and the appends before the String operand do.)
//  4. `null`, `undefined` and `true` have no `+` of their own: they go to
//     ToNumber on the numeric branch (null is 0, undefined is NaN, true is 1)
//     and to ToString on the String branch, and which branch is taken depends
//     only on where the String operand sits.
//  5. A Symbol operand refuses BOTH branches (6.1.5.1), and the refusal names
//     the branch it happened on: ToString on a String accumulator, ToNumber on
//     a numeric one. A BigInt is fine against a String — 13.15.3 settles
//     Strings before it looks at numeric types — and a TypeError against a
//     Number.
//  6. An operand that THROWS half way down abandons the accumulator, and
//     nothing about it is observable afterwards: the next chain is unaffected
//     and the value never reaches the program.
//  7. A template literal is a String operand like any other, and a spine that
//     mixes the two forms is one spine.
//  8. Forty-one operands in a loop, which is where a builder's growth and the
//     collector meet.
//  9. The `MathUtils.generateUUID` shape itself, on a deterministic table and
//     fixed inputs so the whole 36-character answer is derivable here — plus
//     the structural facts (`length`, the version and variant nibbles, the
//     case relation across `toLowerCase`) that hold for any inputs.
// 10. A sealed accumulator is an ORDINARY string. It carries slack past its
//     text, and nothing that reads a string may notice: `length`, `===`,
//     `JSON.stringify`, and use as a property key all have to agree.

// ---- 1. order: an earlier operand's toString runs before a later operand ---
const trace1 = [];
const st = { n: 1 };
const A = {
  toString() {
    trace1.push('A(n=' + st.n + ')');
    return 'A';
  },
};
const src = {
  get later() {
    trace1.push('later');
    st.n = 99;
    return 'L';
  },
};
const out1 = A + '-' + src.later + '-' + A;
console.log(out1);
console.log(trace1.join(','));

// ---- 2. hint Default asks valueOf first; String()/`${}` ask the other way --
const trace2 = [];
const V = {
  valueOf() {
    trace2.push('valueOf');
    return 7;
  },
  toString() {
    trace2.push('toString');
    return 'T';
  },
};
const r2 = V + V + 'x' + V;
const t2 = trace2.join(',');
const sv = String(V);
const tv = `${V}`;
const t2b = trace2.join(',');
console.log(r2);
console.log(t2);
console.log(sv + '|' + tv + '|' + t2b);

// ---- 3. numeric start, String finish; and a spine with no String at all ----
function mixedLit(a, b, d) {
  return a + b + 'x' + d;
}
function numThenStr(a, b, c, d) {
  return a + b + c + d + 'x';
}
function allNum(a, b, c, d) {
  return a + b + c + d;
}
const n3a = mixedLit(1, 2, 3);
const n3b = numThenStr(1, 2, 3, 4);
const n3c = allNum(1, 2, 3, 4);
console.log(n3a + '|' + n3b + '|' + n3c);

// ---- 4. null / undefined / true on each branch ----------------------------
function odd(a, b, c) {
  return a + b + c + 'z';
}
function oddFirst(a, b, c) {
  return 'z' + a + b + c;
}
const l4 = odd(null, undefined, true);
const l4b = oddFirst(null, undefined, true);
console.log(l4 + ' / ' + l4b);

// ---- 5. Symbol refuses both branches, by the name of the branch -----------
const sym = Symbol('s');
function symStr(s) {
  return 'a' + 'b' + s + 'c';
}
function symNum(a, b, s) {
  return a + b + s + 'c';
}
let m1 = 'no throw';
let m2 = 'no throw';
try {
  symStr(sym);
} catch (e) {
  // The CLASS is what 7.1.17 ToString / 7.1.3 ToNumber fix for a Symbol;
  // the message is bronze's own wording and is not pinned.
  m1 = e.name;
}
try {
  symNum(1, 2, sym);
} catch (e) {
  m2 = e.name;
}
console.log(m1);
console.log(m2);

// ---- 6. BigInt: fine against a String, a TypeError against a Number -------
function bigStr(x, y) {
  return x + y + 'q' + x;
}
function bigMix(x, y) {
  return x + y + 'q';
}
const r6 = bigStr(1n, 2n);
let m3 = 'no throw';
try {
  bigMix(1n, 1);
} catch (e) {
  // 13.15.3 step 1.f.i: a TypeError; its message is not the specification's.
  m3 = e.name;
}
console.log(r6 + ' / ' + m3);

// ---- 7. an operand that throws leaves nothing behind ----------------------
const boom = {
  get bad() {
    throw new RangeError('boom');
  },
};
let m4 = 'no throw';
let after = '';
try {
  after = 'a' + 'b' + boom.bad + 'c' + 'd';
} catch (e) {
  m4 = e.name + ': ' + e.message;
}
const ok = 'p' + 'q' + 'r' + 's';
console.log(m4 + '~' + after + '~' + ok + '~' + ok.length);

// ---- 8. template literals are String operands like any other -------------
const x8 = 5;
const t8 = `[${x8}]`;
const r8 = t8 + '-' + t8 + '-' + x8 + `!${x8}`;
console.log(r8);

// ---- 9. forty-one operands, in a loop ------------------------------------
function longChain(o, i) {
  return o.a + '-' + o.b + '-' + o.c + '-' + o.d + '-' + o.e + '-' + o.f + '-' +
    o.g + '-' + o.h + '-' + o.i + '-' + o.j + '-' + o.k + '-' + o.l + '-' +
    o.m + '-' + o.n + '-' + o.o + '-' + o.p + '-' + o.q + '-' + o.r + '-' +
    o.s + '-' + o.t + '-' + i;
}
const letters = 'abcdefghijklmnopqrst';
const wide = {};
for (let k = 0; k < 20; k++) {
  wide[letters[k]] = letters[k] + k;
}
let total = 0;
let last = '';
for (let i = 0; i < 50; i++) {
  const s = longChain(wide, i);
  total += s.length;
  last = s;
}
console.log(total + ' ' + last);

// ---- 10. the generateUUID shape ------------------------------------------
const HEX = '0123456789ABCDEF';
const _lut = [];
for (let i = 0; i < 256; i++) {
  _lut.push(HEX[(i >> 4) & 0xf] + HEX[i & 0xf]);
}

// The nineteen-`+` spine of three.js MathUtils.generateUUID, with the four
// random words supplied instead of drawn, so the answer is a function of this
// file alone.
function uuidFrom(d0, d1, d2, d3) {
  return _lut[d0 & 0xff] + _lut[(d0 >> 8) & 0xff] + _lut[(d0 >> 16) & 0xff] + _lut[(d0 >> 24) & 0xff] + '-' +
    _lut[d1 & 0xff] + _lut[(d1 >> 8) & 0xff] + '-' + _lut[((d1 >> 16) & 0x0f) | 0x40] + _lut[(d1 >> 24) & 0xff] + '-' +
    _lut[(d2 & 0x3f) | 0x80] + _lut[(d2 >> 8) & 0xff] + '-' + _lut[(d2 >> 16) & 0xff] + _lut[(d2 >> 24) & 0xff] +
    _lut[d3 & 0xff] + _lut[(d3 >> 8) & 0xff] + _lut[(d3 >> 16) & 0xff] + _lut[(d3 >> 24) & 0xff];
}

const upper = uuidFrom(0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c);
const lower = upper.toLowerCase();
console.log(upper);
console.log(lower);

// A sealed accumulator is an ordinary string: it carries room past its text
// and nothing that reads one may notice.
const keyed = {};
keyed[upper] = 1;
const firstKey = Object.keys(keyed)[0];
const json = JSON.stringify(upper);
console.log(upper.length + '~' + (upper === '00010203-0405-4607-8809-0A0B0C0D0E0F') + '~' +
  json + '~' + firstKey + '~' + (firstKey === upper));

// The structural facts, which hold for any four words. `_lut[d1>>16 & 0x0f |
// 0x40]` puts 0x4X at byte 6, so the character at index 14 is the version
// nibble `4`; `_lut[d2 & 0x3f | 0x80]` puts 0x80..0xBF at byte 8, so index 19
// is one of 8, 9, a, b.
let seed = 20240101;
function rnd() {
  seed = (seed * 16807) % 2147483647;
  return (seed - 1) / 2147483646;
}
function uuid() {
  const d0 = (rnd() * 0xffffffff) | 0;
  const d1 = (rnd() * 0xffffffff) | 0;
  const d2 = (rnd() * 0xffffffff) | 0;
  const d3 = (rnd() * 0xffffffff) | 0;
  return uuidFrom(d0, d1, d2, d3).toLowerCase();
}
let shapeOk = true;
let dashOk = true;
let versionOk = true;
let variantOk = true;
let count = 0;
for (let i = 0; i < 200; i++) {
  const u = uuid();
  if (u.length !== 36) shapeOk = false;
  if (!/^[0-9a-f-]+$/.test(u)) shapeOk = false;
  if (u[8] !== '-' || u[13] !== '-' || u[18] !== '-' || u[23] !== '-') dashOk = false;
  if (u[14] !== '4') versionOk = false;
  const v = u[19];
  if (v !== '8' && v !== '9' && v !== 'a' && v !== 'b') variantOk = false;
  count++;
}
console.log(shapeOk + ' ' + dashOk + ' ' + versionOk + ' ' + variantOk + ' ' + count);

// Two spines that differ in ONE nibble of one operand, so that "the chain
// really did read every operand" is checked against something derivable rather
// than against the run of a generator.
const zeroes = uuidFrom(0, 0, 0, 0);
console.log(zeroes + ' ' + (zeroes !== uuidFrom(1, 0, 0, 0)));

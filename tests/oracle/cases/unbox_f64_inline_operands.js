// A generic ToNumber (IL unbox.f64) of a value whose type inference does not
// know: a Number is its bits inline, everything else takes the helper. Each
// loop warms the site on doubles first, then hands it every other kind of
// value, so a tier-2 guard armed on the warm-up must deopt and still answer
// ToNumber exactly.

function lenOf(o) { return o.length; }
function half(o) { return o.length * 0.5; }

const arrays = [[1, 2, 3], [], new Array(7), 'abcd', { length: 2.5 }];
let s = 0;
for (let i = 0; i < 3000; i++) s += lenOf(arrays[i % 2]) * 1.5;
console.log('warm', s);

// Every kind of `length` a property read can hand the multiply.
const odd = [
  { length: -0 }, { length: NaN }, { length: Infinity }, { length: -Infinity },
  { length: 5 | 0 }, { length: 0x7fffffff | 0 }, { length: -2147483648 | 0 },
  { length: '12' }, { length: ' 0x10 ' }, { length: '' }, { length: 'x' },
  { length: true }, { length: false }, { length: null }, { length: undefined },
  { length: [] }, { length: [4] }, { length: [1, 2] },
  { length: { valueOf() { return 9; } } },
  { length: { toString() { return '3.25'; } } },
  { length: 1e308 }, { length: 5e-324 }, { length: -5e-324 },
  'string', new Array(3), { },
];
for (const o of odd) {
  const h = half(o);
  console.log(Object.is(h, -0) ? '-0' : String(h));
}

// ToPrimitive runs user code from the unbox: its side effects happen exactly
// once per read, in order.
let calls = 0;
const counted = { length: { valueOf() { calls++; return calls * 10; } } };
let t = 0;
for (let i = 0; i < 2000; i++) t += half(i < 1990 ? arrays[0] : counted);
console.log('counted', t, calls);

// A throw from inside ToNumber (a Symbol, a BigInt) reaches the handler.
for (const bad of [Symbol('s'), 10n, { valueOf() { throw new Error('vo'); } }]) {
  try {
    console.log(half({ length: bad }));
  } catch (e) {
    console.log(e.constructor.name, e instanceof TypeError || e.message);
  }
}

// A loop whose counter's bound is read generically: `q.length` changes
// as the loop pushes and shifts, a double each time.
function drain(q) {
  let n = 0;
  while (q.length > 0) {
    const v = q.shift();
    if (v > 1) { q.push(v - 1); q.push(v - 2); }
    n += q.length * 0.25;
  }
  return n;
}
console.log('drain', drain([10]), drain(['3']), drain([]));

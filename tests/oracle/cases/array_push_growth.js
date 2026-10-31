// Array.prototype.push across a capacity boundary: the element block grows
// on the push that does not fit, so each case pushes past 4, 8, 16, ... and
// then reads everything back.

function show(label, a) {
  console.log(label, a.length, JSON.stringify(a));
}

function tryPush(label, a, ...vs) {
  try {
    const r = a.push(...vs);
    console.log(label, 'ok', r, a.length, JSON.stringify(a));
  } catch (e) {
    console.log(label, e.constructor.name, a.length, JSON.stringify(a));
  }
}

// Numbers, then a value that is not one, at every boundary: the elements
// must read back as what was stored, whichever push grew the block.
for (const n of [3, 4, 5, 8, 9, 16, 17, 33]) {
  const a = [];
  for (let i = 0; i < n; i++) a.push(i * 1.5);
  a.push('s' + n);
  a.push(n);
  let sum = 0;
  for (let i = 0; i < n; i++) sum += a[i];
  console.log('nums', n, a.length, sum, a[n], a[n + 1], typeof a[n]);
}

// Every return value of push is the new length.
{
  const a = [];
  const rs = [];
  for (let i = 0; i < 20; i++) rs.push(a.push(i));
  console.log('returns', rs.join(','));
}

// Objects pushed past many growths keep their identity and contents.
{
  const a = [];
  const keep = [];
  for (let i = 0; i < 3000; i++) {
    const o = { i, s: 'x' + i, inner: [i] };
    a.push(o);
    if (i % 997 === 0) keep.push(o);
  }
  let ok = true;
  for (let i = 0; i < a.length; i++) {
    if (a[i].i !== i || a[i].s !== 'x' + i || a[i].inner[0] !== i) ok = false;
  }
  console.log('objects', a.length, ok, keep.every((o) => a[o.i] === o));
}

// Mixed kinds across growths.
{
  const a = [];
  const vals = [1, 'two', null, undefined, true, 2.5, { k: 1 }, [3], -0, NaN, Infinity, 7n];
  for (let r = 0; r < 4; r++) for (const v of vals) a.push(v);
  console.log('mixed', a.length, a.map((v) => typeof v).join(','));
  console.log('mixed -0', Object.is(a[8], -0), Object.is(a[20], -0), Number.isNaN(a[21]));
}

// shift() leaves head room; pushes after it compact or grow.
{
  const a = [];
  for (let i = 0; i < 8; i++) a.push(i);
  for (let i = 0; i < 5; i++) a.shift();
  for (let i = 0; i < 10; i++) a.push(100 + i);
  show('shift then push', a);
  const q = [];
  for (let i = 0; i < 50; i++) {
    q.push(i);
    if (i % 3 === 2) q.shift();
  }
  show('queue', q);
}

// Holes from a length write: the push lands past them and they stay holes.
{
  const a = [1];
  a.length = 5;
  a.push(9);
  console.log('holes', a.length, 1 in a, 4 in a, 5 in a, a[5], JSON.stringify(a));
  const b = new Array(4);
  b.push('x');
  console.log('new Array(4)', b.length, 0 in b, b[4]);
}

// Several values at once across a boundary.
{
  const a = [1, 2, 3];
  tryPush('multi', a, 4, 5, 6, 7, 8, 9);
  const b = [];
  tryPush('multi empty', b);
}

// Integrity levels, set when the block is exactly full and when it is not.
for (const n of [0, 2, 4, 8]) {
  const a = [];
  for (let i = 0; i < n; i++) a.push(i);
  Object.preventExtensions(a);
  tryPush('preventExtensions ' + n, a, 'p');
  const b = [];
  for (let i = 0; i < n; i++) b.push(i);
  Object.seal(b);
  tryPush('seal ' + n, b, 'p');
  const c = [];
  for (let i = 0; i < n; i++) c.push(i);
  Object.freeze(c);
  tryPush('freeze ' + n, c, 'p');
}

// An array carrying a named property, and one with a named accessor.
{
  const a = [1, 2, 3, 4];
  a.tag = 'named';
  a.push(5, 6);
  a.push(7);
  console.log('named', a.length, a.tag, JSON.stringify(a));
  const b = [1, 2, 3];
  let got = 0;
  Object.defineProperty(b, 'extra', { get() { got++; return 'g'; }, enumerable: false, configurable: true });
  b.push(4);
  b.push(5);
  console.log('accessor', b.length, b.extra, got, b[4], JSON.stringify(b));
}

// A subclass of Array grows the same way.
{
  class Stack extends Array {
    top() { return this[this.length - 1]; }
  }
  const s = new Stack();
  for (let i = 0; i < 10; i++) s.push(i * i);
  console.log('subclass', s.length, s.top(), s instanceof Stack, Array.isArray(s), s.join(','));
}

// Arrays made by other builtins, then grown.
{
  const srcs = [
    ['from', Array.from({ length: 4 }, (_, i) => i)],
    ['slice', [1, 2, 3, 4, 5].slice(1)],
    ['concat', [1, 2].concat([3, 4])],
    ['splice', [1, 2, 3, 4, 5, 6].splice(0, 4)],
    ['map', [1, 2, 3, 4].map((x) => x * 2)],
    ['json', JSON.parse('[1,2,3,4]')],
    ['split', 'a,b,c,d'.split(',')],
    ['of', Array.of(1, 2, 3, 4)],
    ['fill', new Array(4).fill(0)],
  ];
  for (const [name, a] of srcs) {
    for (let i = 0; i < 6; i++) a.push('+' + i);
    show('builtin ' + name, a);
  }
}

// push called on something that is not an array, through call.
{
  const o = { length: 2, 0: 'a', 1: 'b' };
  const r = Array.prototype.push.call(o, 'c', 'd');
  console.log('array-like', r, JSON.stringify(o));
  const args = (function () { return arguments; })(1, 2, 3, 4);
  Array.prototype.push.call(args, 5);
  console.log('arguments', args.length, args[4]);
}

// Interleaved push and pop around a boundary.
{
  const a = [];
  let acc = 0;
  for (let i = 0; i < 100; i++) {
    a.push(i);
    a.push(i + 0.5);
    acc += a.pop();
    if (i % 7 === 0) acc += a.pop();
  }
  console.log('push pop', a.length, acc);
}

// A push whose value is a fresh allocation on every iteration, nested.
{
  const outer = [];
  for (let i = 0; i < 40; i++) {
    const inner = [];
    for (let j = 0; j < i; j++) inner.push({ j, t: [j, i] });
    outer.push(inner);
  }
  let total = 0;
  for (const inner of outer) for (const o of inner) total += o.j + o.t[1];
  console.log('nested', outer.length, total);
}

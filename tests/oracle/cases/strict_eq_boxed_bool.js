// `===` against a boxed Boolean that is not a constant: a bool parameter
// boxed for its default (`update = true`), a comparison's result boxed, and a
// Boolean read back from a field. The bits compare must answer false for
// every non-Boolean (1, 'true', a Boolean object, null, undefined, NaN, an
// Int32 1) and true only for the same Boolean.
class Q {
  constructor() { this.n = 0; }
  set(a, update = true) {
    this.n++;
    if (update === true) return 'U';
    if (update === false) return 'f';
    return '-';
  }
}
const q = new Q();
let out = '';
for (let i = 0; i < 1000; i++) out = out.length < 4 ? out + q.set(i, i % 2 === 0) : out;
console.log('warm', out);
const args = [true, false, 1, 0, 'true', '', new Boolean(true), null, NaN, 1 | 0, [], {}];
let s = q.set(0);
for (const a of args) s += q.set(0, a);
console.log('args', s);

function cmp(x, y) {
  const bx = x > y;
  const by = y < x;
  return (bx === by ? 'S' : 'D') + (bx === (x === 3) ? 's' : 'd');
}
let c = '';
for (let i = 0; i < 6; i++) c += cmp(i, 2);
console.log('cmp', c);

const o = { f: true, g: false };
function eqf(v) { return (o.f === v ? 1 : 0) + (o.g === v ? 2 : 0); }
console.log('field', [true, false, 1, 0, 'true', undefined].map(eqf).join(','));

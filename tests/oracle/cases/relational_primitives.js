// The four relational operators over the primitives ToNumber converts with
// no user code (undefined, null, booleans, numbers incl. NaN, -0, infinities),
// each operand of every kind, through call sites that see every kind (the
// runtime's generic helpers), then against strings and objects with valueOf,
// which take the general path and its conversion order.
const vals = [undefined, null, true, false, 0, -0, 1, -1, 0.5, NaN, Infinity, -Infinity, 2147483648];
const names = ['undefined', 'null', 'true', 'false', '0', '-0', '1', '-1', '0.5', 'NaN', 'Infinity', '-Infinity', '2^31'];
function lt(a, b) { return a < b; }
function gt(a, b) { return a > b; }
function le(a, b) { return a <= b; }
function ge(a, b) { return a >= b; }
const bit = (x) => (x ? '1' : '0');
for (let i = 0; i < vals.length; i++) {
  let row = names[i] + ':';
  for (let j = 0; j < vals.length; j++) {
    const a = vals[i], b = vals[j];
    row += ' ' + bit(lt(a, b)) + bit(gt(a, b)) + bit(le(a, b)) + bit(ge(a, b));
  }
  console.log(row);
}
const order = [];
const obj = (n, v) => ({ valueOf() { order.push(n); return v; } });
console.log(lt(obj('a', 1), obj('b', 2)), gt(obj('c', 1), obj('d', 2)), le(obj('e', 3), obj('f', 3)), ge(obj('g', undefined), obj('h', 0)));
console.log(order.join(''));
console.log(lt('10', '9'), gt('10', 9), le(null, '0'), ge(undefined, 'x'), lt(true, '2'), gt(1n, false), le(undefined, 0n));
let undef;
let hits = 0;
for (let k = 0; k < 1000; k++) if (undef > 0 || undef < 0 || undef >= 0 || undef <= 0) hits++;
console.log(hits);

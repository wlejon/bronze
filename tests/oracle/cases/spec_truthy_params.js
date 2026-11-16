// Joins of `a || b` and `a && b` that are only ever tested: the join holds
// whatever value decided it, and the test asks ToBoolean of it. Partway
// through, the operands turn to values of other kinds (strings, NaN, -0,
// objects, null, undefined, 0n), each with its own truthiness.
function count(list, flags) {
  let n = 0;
  for (let i = 0; i < list.length; i++) {
    const x = list[i];
    if ((x && flags[i]) || (!x && i % 3 === 0)) n++;
    if (x || flags[i] || i === 5) n += 10;
  }
  return n;
}
function chain(a, b, c) {
  let k = 0;
  for (let i = 0; i < 50; i++) {
    if ((a || b) && (b || c) && (c || a)) k++;
    a = b; b = c; c = i & 1;
  }
  return k;
}
const ints = [], flags = [];
for (let i = 0; i < 40; i++) { ints.push(i % 4); flags.push(i % 5 !== 0); }
let total = 0;
for (let r = 0; r < 3000; r++) total += count(ints, flags) + chain(r & 1, r & 2, r & 4);
console.log('warm', total);
const odd = ['', 'a', NaN, -0, 0.5, {}, null, undefined, [], 0n, 1n, true, false, Symbol('s')];
const oflags = [1, 0, 'x', '', null, {}, NaN, -0, 3, undefined, 0, false, true, []];
console.log(count(odd, oflags), count(ints.concat(odd), flags.concat(oflags)));
for (const v of odd) console.log(String(typeof v), chain(v, 0, v), chain(0, v, 1), chain(v, v, v));

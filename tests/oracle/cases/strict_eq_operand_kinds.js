// `===` between two values of unknown type answers an object on the left in one
// compare (il_lowering_dynamic.cpp): two different bit patterns of which the
// left is an object are different values. Strings and BigInts compare by
// content and numbers by value, so those are read only when the left operand
// is not an object. Every pairing of kinds, both ways round, warm.

function same(a, b) { return a === b; }
function differ(a, b) { return a !== b; }

const o1 = { n: 1 };
const o2 = { n: 1 };
const f1 = function () {};
const a1 = [];
const s1 = 'listener';
const s2 = ['list', 'ener'].join('');
const s3 = 'listenex';
const b1 = 12345678901234567890n;
const b2 = BigInt('12345678901234567890');
const b3 = 5n;
const y1 = Symbol('y');

const values = [
  ['o1', o1], ['o2', o2], ['f1', f1], ['a1', a1],
  ['s1', s1], ['s2', s2], ['s3', s3], ['empty', ''],
  ['b1', b1], ['b2', b2], ['b3', b3],
  ['y1', y1], ['zero', 0], ['negzero', -0], ['one', 1], ['half', 0.5],
  ['nan', NaN], ['inf', Infinity], ['true', true], ['false', false],
  ['null', null], ['undef', undefined],
];

for (let warm = 0; warm < 200; warm++) {
  for (let i = 0; i < values.length; i++) {
    same(values[i][1], values[(i + warm) % values.length][1]);
    differ(values[i][1], values[(i + warm) % values.length][1]);
  }
}

for (let i = 0; i < values.length; i++) {
  let row = '';
  for (let j = 0; j < values.length; j++) {
    const eq = same(values[i][1], values[j][1]);
    const ne = differ(values[i][1], values[j][1]);
    row += eq === !ne ? (eq ? '=' : '.') : '?';
  }
  console.log(row, values[i][0]);
}

// The listener loop's shape: an object against an object, mostly different.
function count(list, fn, context) {
  let n = 0;
  for (let i = 0; i < list.length; i++) {
    if (list[i].fn !== fn || (context && list[i].context !== context)) n++;
  }
  return n;
}
const list = [];
for (let i = 0; i < 64; i++) list.push({ fn: i % 3 === 0 ? f1 : same, context: i % 2 === 0 ? o1 : o2 });
let total = 0;
for (let r = 0; r < 500; r++) total += count(list, f1, r % 2 === 0 ? o1 : undefined);
console.log('count', total, count(list, 'f1', o1), count(list, f1, s1), count(list, same, o2));

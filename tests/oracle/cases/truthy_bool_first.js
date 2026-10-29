// ToBoolean of a dynamic value whose site mostly sees true/false and then
// every other kind: the Boolean arm ahead of the classifier must answer only
// for the two Booleans, and Int32-tagged values (the tag just below Boolean),
// Numbers, Strings, BigInts, objects and the nullish values fall through to
// their own answers.
class Node {
  constructor(flag) {
    this.flag = flag;
    this.other = true;
  }
  test() {
    if (this.flag) return 'T';
    return 'F';
  }
  both() {
    return (this.flag && this.other) ? 'T' : 'F';
  }
}

const n = new Node(true);
let warm = '';
for (let i = 0; i < 2000; i++) {
  n.flag = (i % 3) === 0;
  warm = warm.length < 6 ? warm + n.test() : warm;
}
console.log('warm', warm);

const values = [true, false, 0, -0, 1, -1, 2, 0.5, NaN, Infinity, -Infinity,
  '', 'a', '0', 'false', null, undefined, {}, [], 0n, 1n, Symbol('s'), function () {},
  new Boolean(false), 2147483647, -2147483648, 4294967296];
let line = '';
let line2 = '';
for (const v of values) {
  n.flag = v;
  line += n.test();
  line2 += n.both();
}
console.log('test', line);
console.log('both', line2);

// The same through an int32 counter's boxes and a bool computed in a loop.
let s = '';
for (let i = -2; i <= 2; i++) {
  n.flag = i | 0;
  s += n.test();
  n.flag = i > 0;
  s += n.test();
}
console.log('loop', s);

// Integer-valued results of int-producing operations and int element reads,
// which a runtime may hold in the Int32 box just below the Boolean tag.
const ia = new Int32Array([2, -2, 0, 1, 3]);
const ints = [Math.imul(3, 5), Math.imul(2, 0), 'abc'.length, ''.length, [7, 8].indexOf(9), [7, 8].indexOf(8),
  5 >>> 0, 0 >>> 0, ~~2, 6 & 1, 6 & 2, 'ab'.charCodeAt(1), ia[0], ia[1], ia[2], ia[3], ia[4]];
let t = '';
for (const v of ints) {
  n.flag = v;
  t += n.test();
}
for (let i = 0; i < ia.length; i++) {
  n.flag = ia[i];
  t += n.test();
}
console.log('ints', t);

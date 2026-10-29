// String literals boxed inline from the thread's key-header table: the same
// literal must be the same string every time (a switch on a stored literal,
// a literal used as a key, ===, concatenation), including after the program
// has made thousands of new property keys at run time, which grows the table
// the inline path reads (and so moves it).
function order(o) {
  switch (o) {
    case 'XYZ': return 1;
    case 'YXZ': return 2;
    case 'ZXY': return 3;
    case 'ZYX': return 4;
    case 'YZX': return 5;
    case 'XZY': return 6;
    default: return 0;
  }
}
class E { constructor() { this._order = 'XYZ'; } }
const e = new E();
let sum = 0;
for (let i = 0; i < 3000; i++) sum += order(e._order);
console.log('switch', sum);

const bag = {};
for (let i = 0; i < 5000; i++) bag['key_' + i + '_' + (i * 7)] = i;
console.log('bag', Object.keys(bag).length, bag['key_4999_34993']);

const orders = ['XYZ', 'YXZ', 'ZXY', 'ZYX', 'YZX', 'XZY', 'xyz', ''];
let line = '';
for (const o of orders) line += order(o);
console.log('after', line);

const lit = () => 'hello';
console.log('same', lit() === lit(), lit() === 'hel' + 'lo', ('hel' + 'lo') === lit(), lit().length);
const m = {};
m[lit()] = 1;
m['hello'] += 1;
console.log('key', m.hello, Object.keys(m).join('|'));
const fresh = {};
for (let i = 0; i < 3; i++) fresh['dyn' + i] = 'v' + i;
console.log('dyn', fresh.dyn0, fresh.dyn2, 'dyn1' in fresh, typeof 'x', 'a' < 'b', '' === "");

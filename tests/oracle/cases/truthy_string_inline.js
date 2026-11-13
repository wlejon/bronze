// ToBoolean of a String in a condition: true unless the string is empty,
// whatever made it (a literal, a concatenation, a slice, a template, a
// number's string), and the other kinds a condition meets beside it.

function pick(v) { return v ? 'T' : 'F'; }
function both(a, b) { return (a && b) ? 1 : 0; }
function either(a, b) { return (a || b) ? 1 : 0; }
function not(v) { return !v; }
function count(list) {
    let n = 0;
    for (let i = 0; i < list.length; i++) if (list[i].id) n++;
    return n;
}

let built = '';
const pieces = ['', 'a', '', 'bc'];
const out = [];
for (const p of pieces) {
    built = built + p;
    out.push(pick(built) + built.length);
}
console.log(out.join(' '));

const values = ['', ' ', '0', 'false', 'null', 'x'.slice(1), 'xy'.slice(1), `${''}`, `${0}`, String(''), String(0),
                'abc'.substring(3), 'abc'.charAt(7), 'abc'.charAt(1), [].join(','), [1].join(','), ''.padEnd(0),
                ''.padEnd(2), 'é', '\u0000', 0, -0, NaN, 1, 0.5, null, undefined, true, false, {}, [], 0n, 1n,
                Symbol('s'), () => 0];
console.log(values.map(pick).join(''));
console.log(values.map((v) => (not(v) ? 1 : 0)).join(''));
console.log(values.map((v) => both(v, 'k')).join(''), values.map((v) => both('k', v)).join(''));
console.log(values.map((v) => either(v, '')).join(''), values.map((v) => either('', v)).join(''));

const items = [];
for (let i = 0; i < 40; i++) items.push({ id: i % 3 === 0 ? '' : 'pipe' + (i % 5) });
items.push({ id: 0 }, { id: null }, {}, { id: 'x' });
let total = 0;
for (let r = 0; r < 50; r++) total += count(items);
console.log(count(items), total);

let s = '';
let turns = 0;
while (!s) { s += turns > 2 ? 'done' : ''; turns++; }
console.log(turns, s, s ? s.length : -1, '' ? 1 : 2, 'a' ? 1 : 2);

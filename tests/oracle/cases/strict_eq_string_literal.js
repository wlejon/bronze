// `===` and `!==` with a string LITERAL on one side, which generated code
// lowers on its own (il_box_str_key.cpp lower_strict_eq_string_literal): the
// literal's bits first, then only a String with other bits goes on to the
// length / memoized-hash test and the contents compare. Every operand kind
// must still reach its own answer: the literal itself, equal text built every
// way a string can be built (concatenation, builders still growing, slices of
// a two-byte string, case maps, JSON, templates, joins), strings of one length
// and one hash slot that differ, and every non-String kind, including the
// ones whose ToString or ToPrimitive is the literal's text.

function isXYZ(v) { return v === 'XYZ'; }
function xyzIs(v) { return 'XYZ' === v; }
function notXYZ(v) { return v !== 'XYZ'; }
function isEmpty(v) { return v === ''; }
function isE(v) { return v === 'été'; }
function isJa(v) { return v === '日本'; }

function order(v) {
  switch (v) {
    case 'XYZ': return 1;
    case 'YXZ': return 2;
    case 'ZXY': return 3;
    case 'ZYX': return 4;
    case 'YZX': return 5;
    case 'XZY': return 6;
    case '1': return 7;
    case '': return 8;
    default: return 0;
  }
}

function fallthrough(v) {
  let out = '';
  switch (v) {
    case 'a': out += 'a';
    case 'b': out += 'b'; break;
    case 'c': out += 'c';
    default: out += 'd';
  }
  return out;
}

let grow = '';
for (const ch of ['X', 'Y', 'Z']) grow += ch;
let grown = 'XY';
grown += 'Z';
const twoByte = '日XYZ'.slice(1);
const keyed = {};
const used = ['X', 'Y', 'Z'].join('');
keyed[used] = 1;  // its hash memoized as a key
const tpl = `${'X'}Y${'Z'}`;

const subjects = [
  ['literal', 'XYZ'],
  ['concat', 'X' + 'YZ'],
  ['builder loop', grow],
  ['builder +=', grown],
  ['two-byte slice', twoByte],
  ['hashed key', used],
  ['template', tpl],
  ['upper', 'xyz'.toUpperCase()],
  ['json', JSON.parse('"XYZ"')],
  ['fromCharCode', String.fromCharCode(88, 89, 90)],
  ['repeat', 'XYZ'.repeat(1)],
  ['substring', 'AXYZB'.substring(1, 4)],
  ['split join', 'X-Y-Z'.split('-').join('')],
  ['reversed twice', 'XYZ'.split('').reverse().reverse().join('')],
  ['same length', 'XZY'],
  ['same length built', ['X', 'Z', 'Y'].join('')],
  ['prefix', 'XY'],
  ['longer', 'XYZW'],
  ['lower', 'xyz'],
  ['empty', ''],
  ['wrapper', new String('XYZ')],
  ['toString object', { toString() { return 'XYZ'; } }],
  ['valueOf object', { valueOf() { return 'XYZ'; } }],
  ['array', ['XYZ']],
  ['number', 1],
  ['nan', NaN],
  ['negzero', -0],
  ['bigint', 1n],
  ['symbol', Symbol('XYZ')],
  ['undefined', undefined],
  ['null', null],
  ['true', true],
  ['false', false],
  ['function', function XYZ() {}],
];

for (const [label, v] of subjects) {
  console.log(label, isXYZ(v), xyzIs(v), notXYZ(v), isEmpty(v), order(v));
}

// The growing builder: compared, then grown past the literal, then compared.
let b = 'XY';
b += 'Z';
console.log('builder before', isXYZ(b), order(b));
b += 'W';
console.log('builder after', isXYZ(b), order(b), b === 'XYZW');

// Two-byte and Latin-1 literals against equal text of the other width.
console.log('latin1', isE('été'), isE('é' + 'té'), isE('日été'.slice(1)),
            isE('ete'), isE('étè'));
console.log('two-byte', isJa('日本'), isJa('日' + '本'), isJa('x日本'.slice(1)),
            isJa('日'), isJa('本日'));

// Empty literal against every falsy kind and an empty string built late.
console.log('empty', isEmpty(''), isEmpty('a'.slice(1)), isEmpty([].join()), isEmpty(0), isEmpty(false),
            isEmpty(null), isEmpty(undefined), isEmpty(NaN), isEmpty(0n), isEmpty([]));

// Numeric text against the number it spells.
console.log('numeric', order('1'), order(1), order(String(1)), order(1 + ''), order('01'));

console.log('fallthrough', fallthrough('a'), fallthrough('b'), fallthrough('c'), fallthrough('e'),
            fallthrough('a'.toLowerCase()), fallthrough(['b'].join('')));

// An Euler-shaped class: the literal reaches the field through a default
// parameter, a setter, a copy from another instance and a built string, and
// the switch reads it back through the field.
class Euler {
  constructor(x = 0, y = 0, z = 0, order = 'XYZ') {
    this._x = x; this._y = y; this._z = z; this._order = order;
  }
  set order(v) { this._order = v; }
  get order() { return this._order; }
}
function quat(e) {
  const c = Math.cos(e._x / 2);
  switch (e._order) {
    case 'XYZ': return c * 1;
    case 'YXZ': return c * 2;
    case 'ZXY': return c * 3;
    default: return -1;
  }
}
const e1 = new Euler(1, 2, 3);
const e2 = new Euler(1, 2, 3, 'YXZ');
const e3 = new Euler(1, 2, 3, ['Z', 'X', 'Y'].join(''));
const e4 = new Euler(1, 2, 3);
e4.order = 'X' + 'YZ';
const e5 = new Euler(1, 2, 3, e2.order);
const e6 = new Euler(1, 2, 3, 'xyz');
let acc = 0;
for (let i = 0; i < 3000; i++) {
  acc += quat(e1) + quat(e2) + quat(e3) + quat(e4) + quat(e5) + quat(e6);
  if (i === 1500) e1.order = 'ZXY';
}
console.log('euler', acc.toFixed(6), quat(e1), quat(e2), quat(e3), quat(e4), quat(e5), quat(e6));

// A hot loop so the tiers that compile late see every kind too.
let hits = 0, misses = 0;
for (let i = 0; i < 4000; i++) {
  const v = subjects[i % subjects.length][1];
  if (isXYZ(v)) hits++; else misses++;
  if (order(v) === 2) hits += 100;
}
console.log('loop', hits, misses);

// Inside try/catch and a generator, and with the literal also used as a key.
function* gen(list) { for (const v of list) yield v === 'XYZ' ? 'y' : 'n'; }
console.log('gen', [...gen(['XYZ', 'X' + 'YZ', 'XZY', 1])].join(''));
try {
  const o = { XYZ: 'XYZ' };
  console.log('try', o.XYZ === 'XYZ', o['X' + 'YZ'] === 'XYZ', Object.keys(o)[0] === 'XYZ');
  throw 'XYZ';
} catch (err) {
  console.log('caught', err === 'XYZ', err !== 'XYZ');
}

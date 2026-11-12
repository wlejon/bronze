// Members read off primitives: index and length on strings, inherited
// methods, and names that are not there.
const s = 'héllo';
console.log(s[0], s[1], s[4], s[5], s[-1], s['1'], s['01'], s[1.5], s.length, s['length']);
const keys = ['length', '0', '4', '5', 'at', 'missing', 'constructor', 'toString', '__proto__', 'trim'];
for (const k of keys) {
    const v = s[k];
    console.log(k, typeof v === 'function' ? 'function ' + v.name : typeof v === 'object' ? 'object' : v);
}
const sym = Symbol('own');
console.log(s[sym], typeof s[Symbol.iterator], (5)[sym], true[sym]);

for (const p of [42, 1.5, true, false, Symbol('d'), 10n]) {
    console.log(typeof p, typeof p.toString, typeof p.valueOf, p.missing, p['length'], p[0],
        p.constructor.name, typeof p.hasOwnProperty);
}
console.log((42).toFixed(1), (42)['toFixed'](2), true.toString(), 'abc'.toUpperCase(), 'abc'['charAt'](1));
console.log(Symbol('desc').description, Symbol.iterator.description, Symbol().description);

// Numeric-looking and long keys on a string.
const long = 'x'.repeat(300);
console.log(long[299], long[300], long['299'], long.length, long['4294967295'], long['-0'], long[1e3]);
const wide = '你好😀';
console.log(wide.length, wide[0], wide[1], wide.charCodeAt(2), wide[3].charCodeAt(0), wide[4]);

// A computed key that is an object is converted once.
let conversions = 0;
const key = { toString() { conversions++; return 'length'; } };
console.log('abcd'[key], (7)[key], conversions);

// Writes to a primitive are dropped (sloppy) and reads see nothing.
const prim = 'str';
prim.added = 1;
console.log(prim.added, prim.length);

// null and undefined still throw, naming the member.
for (const base of [null, undefined]) {
    try { base.member; } catch (e) { console.log(e instanceof TypeError); }
    try { base['computed']; } catch (e) { console.log(e instanceof TypeError); }
}

// Templates evaluated again and again with the same pieces and with equal
// pieces built different ways: the answer is the same text across
// collections, with surrogates, two-byte and Latin-1 pieces mixed, very long
// results, numbers of every kind, and the results used as property keys, Map
// keys and compared with strings built another way.

function key(a, b, c) { return `${a}-${b}:${c}`; }

// The same pieces, again and again, with garbage between (collections move
// and free the pieces' first copies).
let acc = 0;
let junk = [];
for (let round = 0; round < 40; round++) {
    const k = key('style', round % 4, 1);
    acc += k.length + k.charCodeAt(k.length - 1);
    for (let j = 0; j < 2000; j++) junk.push({ j, s: 'x' + j });
    if (junk.length > 20000) junk = [];
}
console.log('repeat', acc, key('style', 3, 1));

// Pieces that are equal text but different strings, and strings built by
// different routes: every answer compares equal and hashes alike.
const parts = ['al', 'pha'];
const built = parts.join('');
const sliced = 'xalphax'.slice(1, 6);
const a1 = `${built}|${7}`;
const a2 = `${sliced}|${7}`;
const a3 = 'alpha' + '|' + 7;
console.log('equal', a1 === a2, a2 === a3, a1 === 'alpha|7');
const obj = {};
obj[a1] = 1; obj[a2] = (obj[a2] || 0) + 1; obj[a3] = (obj[a3] || 0) + 1;
console.log('keys', Object.keys(obj).length, obj['alpha|7']);
const m = new Map();
for (let i = 0; i < 5; i++) {
    const k = `${built}|${7}`;
    m.set(k, (m.get(k) || 0) + 1);
}
m.set(a3, m.get(a3) + 10);
console.log('map', m.size, m.get('alpha|7'));

// Two-byte and Latin-1 pieces mixed, surrogate pairs split across pieces and
// whole in one.
const hi = '\uD83D', lo = '\uDE00';
const smile = `${hi}${lo}`;
const smile2 = `${hi}${lo}`;
console.log('surrogate', smile === '\u{1F600}', smile2 === smile, smile.length, smile.codePointAt(0));
const mix = (n) => `é${n}€${'ü'.repeat(2)}${hi}`;
const r1 = mix(5), r2 = mix(5), r3 = mix(6);
console.log('mix', r1 === r2, r1 === r3, r1.length, r1.charCodeAt(r1.length - 1), [...r1.slice(0, 5)].map(c => c.charCodeAt(0)).join(','));
const lone = `${lo}${hi}`;
console.log('lone', lone.length, lone.charCodeAt(0), lone.charCodeAt(1), lone === lo + hi);

// Numbers of every kind as pieces: small, large, negative, fractions, -0,
// NaN, infinities, and the same value boxed differently.
const nums = [0, -0, 1, 1023, 1024, 65536, -5, 0.5, 1e21, 1e-7, NaN, Infinity, -Infinity, 2 ** 31, 2 ** 53];
let ns = '';
for (let pass = 0; pass < 3; pass++) {
    for (const n of nums) ns += `[${n}]`;
}
console.log('nums', ns.length, ns.slice(0, 120));
let half = 3;
const viaInt = `${half}`, viaDouble = `${half * 1.0}`, viaDiv = `${6 / 2}`;
console.log('same number', viaInt === viaDouble, viaDouble === viaDiv);

// Very long results, repeated and extended.
const long = 'L'.repeat(300);
const big1 = `${long}:${1}`, big2 = `${long}:${1}`;
let grown = `${big1}`;
for (let i = 0; i < 5; i++) grown = `${grown}${i}`;
console.log('long', big1 === big2, big1.length, grown.length, grown.slice(-8), big1.slice(298, 303));
const longTwo = `${'ж'.repeat(200)}${long}`;
console.log('long two-byte', longTwo.length, longTwo === `${'ж'.repeat(200)}${long}`, longTwo.charCodeAt(0), longTwo.charCodeAt(250));

// A `+` chain's value as a piece, and a template inside a chain.
function chained(x, y) { return `<${x + '/' + y}>` + `${x}` + '!'; }
console.log('chain', chained('a', 'b'), chained('a', 'b') === chained('a', 'b'), chained(1, 2));

// Objects as pieces run their toString every time, even with the same object.
let calls = 0;
const counter = { toString() { calls++; return 'c' + calls; } };
const t1 = `${counter}`, t2 = `${counter}`;
console.log('toString', t1, t2, calls);

// Many distinct templates, then the first ones again: evicted or kept, the
// text is the same.
const first = [];
for (let i = 0; i < 3000; i++) {
    const s = `${i % 37}-${'k'}-${i}`;
    if (i < 10) first.push(s);
}
let again = 0;
for (let i = 0; i < 10; i++) if (`${i % 37}-${'k'}-${i}` === first[i]) again++;
console.log('evicted', again, first[9]);

// A template with two pieces or more is one concat.n (bronze_concat_template):
// a Number substitution reaches it unconverted and is formatted there, a
// small integer as the cached string. These pin the conversions at the edges
// of that cache, the encodings of the result, the switch to a view at 256
// units, and that a joined string is an ordinary key: equal to, and hashing
// like, the same text built any other way.
const nums = [0, -0, 1, 9, 10, 1023, 1024, 4294967295, 4294967296, -1, 0.5, -0.25, 1e21, 1e-7, NaN, Infinity, -Infinity, 2 ** 53, 123.456];
console.log(nums.map((n) => `${n}`).join(' '));
console.log(nums.map((n) => `<${n}>`).join(' '));
console.log(nums.map((n) => `${n}${n}`).join(' '));
console.log(`${1}${2}${3}`, `${-0}px`, `px${-0}`, `${0.1 + 0.2}`);

// Encodings: Latin-1 only, Latin-1 above 0x7f, UTF-16, a surrogate pair split
// across two substitutions, and empty pieces between them.
const hi = '\xff\xe9', bmp = '日本', pair = '😀';
const lone = pair[0], tail = pair[1];
const a = `${hi}|${bmp}|${hi}`;
const b = `${lone}${tail}`;
console.log(a, a.length, a.charCodeAt(0), a.charCodeAt(3), b === pair, b.length, b.codePointAt(0));
console.log(`${''}${''}`.length, `${''}x${''}`, `${hi}${1}${bmp}${2}`, `${pair}${7}`.length);

// Lengths around the view threshold (256 units), in both encodings.
for (const n of [250, 254, 255, 256, 257, 300]) {
    const s = 'a'.repeat(n - 4);
    const t = `${s}${1}${2}${3}${4}`;
    const u = `${bmp.repeat((n - 2) / 2 | 0)}${1}${2}`;
    console.log(n, t.length, t.slice(-6), u.length, u.slice(-3), t === s + '1234');
}
// A long join extended in a loop stays correct.
let acc = '';
for (let i = 0; i < 2000; i++) acc = `${acc}${i % 10}`;
console.log(acc.length, acc.slice(0, 12), acc.slice(-12));

// Joined strings as property keys: written under one spelling, read under
// others; deleted; used in a Map and a Set; compared.
const o = Object.create(null);
for (let i = 0; i < 300; i++) o[`k${i}-${i * 7}:${i % 3}`] = i;
let hits = 0;
for (let i = 0; i < 300; i++) {
    const viaPlus = 'k' + i + '-' + (i * 7) + ':' + (i % 3);
    const viaArr = ['k', i, '-', i * 7, ':', i % 3].join('');
    if (o[viaPlus] === i && o[viaArr] === i && viaPlus === `k${i}-${i * 7}:${i % 3}`) hits++;
}
for (let i = 0; i < 300; i += 2) delete o[`k${i}-${i * 7}:${i % 3}`];
console.log(hits, Object.keys(o).length, `k${3}-${21}:${0}` in o, `k${4}-${28}:${1}` in o);
const m = new Map(), set = new Set();
for (let i = 0; i < 50; i++) { m.set(`${i}.${i}`, i); set.add(`${i % 5}${'x'}`); }
console.log(m.get('7.7'), m.get(`${7}.${7}`), set.size, set.has('3x'), m.has(`${7}${'.'}${7}`));
const plain = { [`a${1}`]: 1, [`b${2}`]: 2 };
console.log(JSON.stringify(plain), plain.a1 + plain.b2, `${'a'}${1}` === 'a1');

// A substitution that is an object converts with the string hint, a Symbol
// throws at its place, and a boxed Number is an object.
const both = { toString() { return 'T'; }, valueOf() { return 42; } };
console.log(`${both}${1}`, `${new Number(5)}${new String('s')}`, `${[1, 2]}${{}}`, `${null}${undefined}${true}`);
try { console.log(`${1}${Symbol('s')}`); } catch (e) { console.log(e.constructor.name); }
console.log(`${10n}${-3n}`, `${1}${'é'}${2}`.normalize('NFD').length);

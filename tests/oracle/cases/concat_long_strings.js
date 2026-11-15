// Long strings built by concatenation: every result must match node.
const out = [];
function h(s) { let x = 0; for (let i = 0; i < s.length; i++) x = (x * 31 + s.charCodeAt(i)) | 0; return x; }
function rec(name, s) { out.push(name + ':' + s.length + ':' + h(s)); }

// 1. plain loop append
let s = '';
for (let i = 0; i < 5000; i++) s += 'ab' + i;
rec('loop', s);

// 2. aliasing: an older value must not see later appends
let a = '';
for (let i = 0; i < 300; i++) a += 'x';
const snap = a;
a += 'TAIL1';
let b = snap + 'TAIL2';
let c = snap + 'T3';
rec('snap', snap); rec('a', a); rec('b', b); rec('c', c);
out.push(a === snap + 'TAIL1', b === snap + 'TAIL2', a !== b, snap.length);

// 3. branch from a non-tip view repeatedly
let base = 'q'.repeat(400);
let v1 = base + '1'; let v2 = v1 + '2'; let v3 = v1 + '3'; let v4 = v2 + '4'; let v5 = v3 + '5';
out.push(v1.slice(-3), v2.slice(-3), v3.slice(-3), v4.slice(-4), v5.slice(-4));

// 4. Latin-1 to UTF-16 in the middle
let u = '';
for (let i = 0; i < 400; i++) u += 'z';
const before = u;
u += '中文';
u += 'tail';
for (let i = 0; i < 300; i++) u += (i % 7 === 0 ? 'é' : 'k') + 'Ж';
rec('u', u); rec('before', before);
out.push(u.indexOf('文'), u.charCodeAt(401), u.slice(398, 406), before === 'z'.repeat(400));

// 5. as keys, in Map/Set, equality with a flat string of the same text
const flat = Array.from({ length: 300 }, (_, i) => 'k' + (i % 10)).join('');
let built = '';
for (let i = 0; i < 300; i++) built += 'k' + (i % 10);
const o = {}; o[built] = 1; o[flat] = (o[flat] || 0) + 1;
const m = new Map(); m.set(built, 'v'); const st = new Set([built, flat]);
out.push(built === flat, o[flat], Object.keys(o).length, m.get(flat), st.size, built == flat, built < flat, built <= flat);

// 6. string methods over a view
out.push(built.indexOf('k9k0'), built.lastIndexOf('k1'), built.includes('k5k6k7'), built.startsWith('k0k1'),
  built.endsWith('k9'), built.substring(10, 20), built.slice(-6), built.charAt(599), built[1], built.at(-1),
  built.split('k').length, built.replace('k5', 'ZZ').length, built.toUpperCase().slice(0, 8), built.trim().length,
  built.padEnd(610, '-').slice(-12), built.codePointAt(3), built.localeCompare(flat), built.concat('!').length,
  /k9(k0)/.exec(built).index, built.match(/k3/g).length, built.replaceAll('k', '').length,
  JSON.stringify(built).length, JSON.parse(JSON.stringify({ s: built })).s === built,
  encodeURIComponent(built).length, parseInt('12' + built), Number('1' + '0'.repeat(300)), [...built].length,
  built.normalize() === built, built.repeat(2).length, (built + 5).slice(-3), (5 + built).slice(0, 3),
  `${built}!${built}`.length, (built + null).slice(-4), (built + undefined).length, (built + true).slice(-4),
  (built + 1.5).slice(-3), (built + {}).slice(-15), (built + [1, 2]).slice(-3), String(built) === built,
  built.toString().length, built.valueOf() === flat, typeof built, new String(built).length, Symbol(built).description.length);

// 7. chains: s = s + a + b, prepend, and numbers
let ch = '';
for (let i = 0; i < 2000; i++) ch = ch + 'a' + i + ',' + (i * 0.5);
rec('chain', ch);
let pre = '';
for (let i = 0; i < 1000; i++) pre = i + ':' + pre;
rec('prepend', pre);
let num = '';
for (let i = 0; i < 3000; i++) num += i;
rec('num', num);
let mixed = '';
for (let i = 0; i < 1500; i++) { mixed += 'm'; if (i % 100 === 0) { const t = mixed; mixed = t + '|' + t.length; } }
rec('mixed', mixed);

// 8. two strings growing from one buffer in turn
let p = 'p'.repeat(300), q = p;
for (let i = 0; i < 500; i++) { p += 'P'; q += 'Q' + i; }
rec('p', p); rec('q', q);

// 9. array join / property names / template of views, and garbage in between
const parts = [];
for (let i = 0; i < 50; i++) { let t = ''; for (let j = 0; j < 40; j++) t += 'part' + i + '_' + j; parts.push(t); }
rec('join', parts.join('|'));
let big = '';
for (let i = 0; i < 200000; i++) { big += 'y'; if ((i & 8191) === 0) { const junk = []; for (let k = 0; k < 2000; k++) junk.push({ k, s: 'j' + k }); } }
rec('big', big);
const keyed = {}; for (let i = 0; i < 20; i++) { let k = ''; for (let j = 0; j < 30; j++) k += 'key_part_' + (i % 5) + '_'; keyed[k] = (keyed[k] || 0) + 1; }
out.push(Object.keys(keyed).length, Object.values(keyed).join(','));

console.log(out.join('\n'));

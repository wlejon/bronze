// trim / trimStart / trimEnd: the ends are found on the string itself, a
// string with nothing to strip is returned as it is, and every WhiteSpace and
// LineTerminator code unit counts (and nothing else does).
const out = [];
const show = (s) => JSON.stringify(s).replace(/[\u007f-￿]/g, (c) => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'));
const ws = ['\t', '\n', '\v', '\f', '\r', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    ' ', ' ', '　', '﻿'];
const notWs = ['​', '\u0085', '᠎', '⁠', '\0', '\u001f', '¡', ' x', '_'];
for (const w of ws) {
    const s = w + 'a' + w + 'b' + w + w;
    out.push([show(s.trim()), show(s.trimStart()), show(s.trimEnd()), (w + w).trim().length].join(' '));
}
for (const w of notWs) {
    const s = w + 'a' + w;
    out.push([show(s.trim()), s.trim() === s, s.trimStart().length, s.trimEnd().length].join(' '));
}
// Nothing to strip: the same string comes back, of either width.
const plain = ['', 'a', 'serif', 'sans-serif', 'a b', 'Жи', '😀', 'x'.repeat(300)];
for (const s of plain) out.push([s.trim() === s, s.trimStart() === s, s.trimEnd() === s, s.trim().length].join(' '));
// All whitespace, one side only, wide strings narrowed by the trim, long runs.
out.push(show('   '.trim()), show(' Ж '.trim()), show('　abc　'.trim()), show('　abc　'.trimEnd()));
out.push(show('  left'.trimStart()), show('  left'.trimEnd()), show('right  '.trimEnd()), show('right  '.trimStart()));
out.push((' '.repeat(1000) + 'mid' + '\n'.repeat(1000)).trim(), (' '.repeat(70000) + 'm').trim().length);
// A piece of a split, a slice and a concatenation trim alike.
const piece = ' a , b ,c '.split(',').map((p) => p.trim()).join('|');
out.push(piece, ('  ab' + 'cd  ').trim(), '--  x  --'.slice(2, 7).trim());
// Receivers that are not strings convert first; null and undefined throw.
const trim = String.prototype.trim;
out.push(show(trim.call(12)), show(trim.call(true)), show(trim.call([' a ', ' b '])), show(trim.call({ toString() { return '\n obj \n'; } })));
out.push(show(trim.call(new String('  boxed '))), show(String.prototype.trimStart.call(-0)), show(String.prototype.trimEnd.call(1.50)));
for (const bad of [null, undefined]) {
    try { trim.call(bad); out.push('no throw'); } catch (e) { out.push(e instanceof TypeError); }
}
try { trim.call(Symbol('s')); out.push('no throw'); } catch (e) { out.push(e instanceof TypeError); }
// One site, many calls and receivers.
function t(s) { return s.trim(); }
let n = 0;
const mix = [' a', 'b ', 'c', '  ', '', '\td\t'];
for (let i = 0; i < 600; i++) n += t(mix[i % mix.length]).length;
out.push(n, 'x'.trim('ignored'), ' y '.trimStart(1, 2).length);
console.log(out.join('\n'));

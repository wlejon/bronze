// A regular expression literal evaluated again is a fresh object with the
// same source and flags; constructor calls with the same text share nothing
// observable; a program evicted from the compiled-pattern cache still runs.
'use strict';
function make() { return /a(b)?c/g; }
const r1 = make();
const r2 = make();
console.log(r1 === r2, r1.source, r1.flags, r1.lastIndex);
r1.exec('xxabcxx');
console.log(r1.lastIndex, r2.lastIndex);

function slash() { return /a\/b[/]c/; }
console.log(slash().source, slash().test('a/b/c'), String(slash()));
function empty() { return new RegExp(''); }
console.log(empty().source, String(empty()));
function newline() { return new RegExp('\n', 'm'); }
console.log(JSON.stringify(newline().source), newline().test('x\ny'));

const flagsOrder = ['gi', 'ig', 'g', '', 'y', 'gimsuy'.replace('u', '')];
for (const f of flagsOrder) {
  const r = new RegExp('ab', f);
  console.log(JSON.stringify(f), r.flags, r.global, r.ignoreCase, r.sticky, r.test('xAB'));
}
for (const f of ['gg', 'q']) {
  try { new RegExp('ab', f); console.log('no throw', f); } catch (e) { console.log(e.constructor.name, f); }
}
try { new RegExp('(', ''); } catch (e) { console.log(e.constructor.name); }
try { new RegExp('(', ''); } catch (e) { console.log(e.constructor.name, 'again'); }

function words(s) { return s.split(/\s+/).filter(w => /^[a-z]+$/i.test(w)); }
let total = 0;
for (let i = 0; i < 3000; i++) total += words('alpha beta 12 Gamma').length;
console.log('words', total);

// Fill the compiled-pattern cache past its capacity so the literal's
// program is evicted, then evaluate the literal again.
const before = make();
for (let i = 0; i < 1500; i++) new RegExp('p' + i + '(x)?', 'g').test('p' + i);
const after = make();
console.log(after.source, after.flags, after.exec('zabc')[0], before.exec('zzac')[0]);
console.log(make().exec('abc').index, make().test('ac'), make().test('ab'));

const sticky = /o/y;
console.log(sticky.test('oo'), sticky.lastIndex);
function stickyLit() { return /o/y; }
const s1 = stickyLit();
s1.test('oo');
console.log(s1.lastIndex, stickyLit().lastIndex);
for (const s of ['a/b[/]c', '[]/]', '[^/]+/', '\\[/]', '[\\]/]/', 'x[a-z/]y/z', '[/', '/[']) {
  try { const r = new RegExp(s); console.log(JSON.stringify(s), r.source, String(r)); } catch (e) { console.log(JSON.stringify(s), e.constructor.name); }
}

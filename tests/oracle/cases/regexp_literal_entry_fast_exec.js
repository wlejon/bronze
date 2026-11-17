// A regular expression literal is a fresh object per evaluation, and exec /
// test sites answer through the fast entries only for a RegExp at the
// intrinsic instance shape.
const seen = [];
for (let i = 0; i < 3; i++) {
  const r = /a(b)?/g;
  seen.push(r);
  console.log(r.lastIndex, r.exec('xab ab').index, r.lastIndex, r.source, r.flags);
}
console.log(seen[0] === seen[1], seen[1] === seen[2], Object.getPrototypeOf(seen[0]) === RegExp.prototype);
function make() { return /x/y; }
const m1 = make(), m2 = make();
m1.lastIndex = 3;
console.log(m1 !== m2, m1.lastIndex, m2.lastIndex, m1.sticky, m2.flags);
{
  const RegExp = function () { return { fake: true }; };
  const r = /q/i;
  console.log(r instanceof globalThis.RegExp, r.fake, r.test('Q'));
}

function exec(re, s) { return re.exec(s); }
function test(re, s) { return re.test(s); }
function show(m) { return m === null ? 'null' : JSON.stringify([...m]) + ' ' + m.index + ' ' + JSON.stringify(m.groups) + ' ' + (m.input === undefined ? '-' : m.input.length); }

const g = /(\d+)/g;
for (let i = 0; i < 4; i++) console.log('g', show(exec(g, 'a1 b22 c333')), g.lastIndex);
const y = /\d/y;
console.log('y', test(y, '12a'), y.lastIndex, test(y, '12a'), y.lastIndex, test(y, '12a'), y.lastIndex);
const named = /(?<word>[a-z]+)-(?<num>\d+)?/;
for (let i = 0; i < 2; i++) console.log('named', show(exec(named, '-- abc-12 ')), show(exec(named, 'xy-')));
const dflag = /b(c)?/d;
const dm = exec(dflag, 'abcab');
console.log('d', JSON.stringify(dm.indices), JSON.stringify(exec(dflag, 'ab').indices));
const lw = /o/g;
lw.lastIndex = '2';
console.log('lastIndex string', test(lw, 'foo'), lw.lastIndex);
lw.lastIndex = { valueOf() { return 1; } };
console.log('lastIndex object', show(exec(lw, 'foo')), lw.lastIndex);
lw.lastIndex = 10;
console.log('lastIndex past end', test(lw, 'foo'), lw.lastIndex);
console.log('non-string argument', test(/1/, 12), show(exec(/null/, null)), test(/undefined/), show(exec(/u/, { toString() { return 'tu'; } })));

// Receivers off the instance shape.
class Sub extends RegExp { exec(s) { this.calls = (this.calls || 0) + 1; return super.exec(s); } }
const sub = new Sub('b+', 'g');
for (let i = 0; i < 3; i++) console.log('subclass', show(exec(sub, 'abbcb')), test(sub, 'abbcb'), sub.calls);
const own = /z/;
own.exec = function () { return ['own']; };
console.log('own exec', show(exec(own, 'zzz')), test(own, 'zzz'));
const own2 = /z/;
own2.test = () => 'own test';
console.log('own test', test(own2, 'a'), show(exec(own2, 'z')));
const notRe = { exec(s) { return 'plain ' + s; }, test(s) { return 'plain test ' + s; } };
console.log('plain', exec(notRe, 'q'), test(notRe, 'q'));
for (const bad of [RegExp.prototype, Object.create(RegExp.prototype)]) {
  try { exec(bad, 'a'); console.log('no throw'); } catch (e) { console.log('exec', e.constructor.name); }
  try { test(bad, 'a'); console.log('no throw'); } catch (e) { console.log('test', e.constructor.name); }
}
const frozen = Object.freeze(/f/g);
try { test(frozen, 'f'); console.log('no throw'); } catch (e) { console.log('frozen', e.constructor.name); }
try { exec(frozen, 'x'); console.log('no throw'); } catch (e) { console.log('frozen miss', e.constructor.name); }
const nonG = Object.freeze(/f/);
console.log('frozen non-global', test(nonG, 'f'), show(exec(nonG, 'af')));
// The cache keeps answering once many patterns have pushed the first out.
const first = /^first(\d)$/;
for (let i = 0; i < 700; i++) new RegExp('p' + i + '(x)?', i % 2 ? 'g' : '');
console.log('after eviction', show(exec(first, 'first7')), test(first, 'first8'), test(first, 'second'));
console.log('generic test', RegExp.prototype.test.call({ exec() { return {}; } }, 'x'), RegExp.prototype.test.call({ exec() { return null; } }, 'x'));
try { RegExp.prototype.test.call({ exec() { return 1; } }, 'x'); console.log('no throw'); } catch (e) { console.log('bad exec result', e.constructor.name); }
try { RegExp.prototype.test.call({ exec: 5 }, 'x'); console.log('no throw'); } catch (e) { console.log('no exec', e.constructor.name); }
function lit() { return /^lit(\d)$/g; }
const before = lit();
for (let i = 0; i < 700; i++) new RegExp('q' + i + '[ab]', 'y');
const after = lit();
console.log('literal after eviction', show(exec(after, 'lit4')), after.lastIndex, test(before, 'lit5'), before.lastIndex, before !== after);

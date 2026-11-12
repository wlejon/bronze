// A search tries the matcher at every index in turn; an index whose unit
// cannot begin a match is skipped without one. The answers are the scan's.
'use strict';
const quoted = /(["'])[^'"]+\1/;
for (const s of ['sans-serif', 'a "Fira Sans" b', "it's 'x'", '', '"', 'serif, "x', "'a'\"b\""]) {
  const m = quoted.exec(s);
  console.log(JSON.stringify(s), quoted.test(s), m && m.index, m && m[0], m && m[1]);
}
const g = /(["'])[^'"]+\1/g;
const text = `say "hi" and 'bye' and "no`;
let m;
while ((m = g.exec(text)) !== null) console.log(m.index, m[0], g.lastIndex);
console.log(g.lastIndex, text.replace(g, '<$&>'), text.split(/(["'])[^'"]+\1/).length);
console.log(text.search(quoted), 'none'.search(quoted), text.match(g));

// Negated classes, units above ASCII, astral characters without `u`.
console.log(/([^a])\1/.exec('aabbc').index, /(é)\1/.exec('xééy').index, /(é)\1/.test('xeey'));
console.log(/([^\x00-\x7f])\1/.exec('ab中中c').index, /(\ud83d)\1/.test('😀'));
console.log(/([\ud83d])\ude00\1/.exec('a😀\ud83d').index);

// Patterns that can match the empty string, the index past the end included.
console.log(/(a)?\1/.exec('xyz').index, /(x)*\1$/.exec('abc').index, /(?:(a)|b)\1$/.exec('xb').index);
console.log(/(?=(b))\1/.exec('abc').index, /\b(c)\1/.exec('a cc').index, /(?<=(a))\1/.exec('baab').index);
console.log(/(?!a)(.)\1/.exec('aabb').index, /^(b)\1/m.exec('a\nbb').index, /(b)\1$/.exec('abb').index);
console.log(/(?:)\1(z)/.exec('az').index, /(|a)\1b/.exec('aab').index);

// `i`, `u`, `v` and `y`: the unit at the index is not what decides.
console.log(/(q)\1/i.exec('aQqb').index, /([k])\1/i.exec('xKk').index, /(K)\1/iu.test('kK'));
console.log(/(.)\1/u.exec('a\u{1f600}\u{1f600}').index, /([\p{L}])\1/v.exec('1aa').index);
const y = /(b)\1/y;
console.log(y.test('abb'), y.lastIndex);
y.lastIndex = 1;
console.log(y.test('abb'), y.lastIndex);

// Many searches of one literal, as a loop that names fonts does.
let hits = 0;
const names = ['serif', '"Open Sans"', 'monospace', "'Courier New'", 'x"y', 'Arial'];
for (let i = 0; i < 600; i++) if (/(["'])[^'"]+\1/.test(names[i % names.length])) hits++;
console.log(hits);

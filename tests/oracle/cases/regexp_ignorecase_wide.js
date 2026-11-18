// Case-insensitive matching over the whole BMP (22.2.2.9 Canonicalize).
// Without `u` it is toUppercase of one code unit, kept only when the result is
// one unit and does not take a non-ASCII unit into ASCII; with `u` (or `v`) it
// is simple case folding. model-viewer's `/^(\-\-|[a-zɀ-￿])/i` needs a class
// range across most of the BMP to fold.

const re = /^(\-\-|[a-zɀ-￿])/i;
console.log(re.test('A'), re.test('一'), re.test('--'), re.test('1'), re.test('Ⱥ'));

console.log(/[a-zɀ-￿]/i.test('Ā'), /[^a-z]/i.test('A'), /[Ā-ſ]/i.test('ā'));
console.log(/[k]/i.test('K'), /[k]/iu.test('K'), /[ß]/i.test('ẞ'), /[ẞ]/i.test('ß'));
console.log(/[ſ]/i.test('s'), /[ſ]/iu.test('s'), /ı/i.test('I'), /i/i.test('İ'));
console.log(/[à-þ]/i.test('À'), /[Ѐ-ӿ]/i.test('ж'), /ა/i.test('Ა'), /Ꭰ/i.test('ꭰ'));
console.log(/[a-zɀ-￿]/iv.test('A'), /[^ɀ-￿]/i.test('a'), /[^ɀ-￿]/i.test('Ⱥ'));
console.log(/ǅ/i.test('Ǆ'), /ǆ/i.test('ǅ'), /ϐ/i.test('Β'), /ẛ/i.test('Ṡ'), /ﬀ/i.test('FF'));

// Without `u`, Canonicalize is the engine's own toUpperCase of one unit, so
// the two must agree everywhere: count the units where they do not.
function canon(ch) {
  const up = ch.toUpperCase();
  if (up.length !== 1) return ch;
  if (ch.charCodeAt(0) >= 128 && up.charCodeAt(0) < 128) return ch;
  return up;
}
let disagree = 0;
for (let u = 0; u <= 0xffff; u++) {
  if (u >= 0xd800 && u <= 0xdfff) continue;
  const ch = String.fromCharCode(u);
  const r = new RegExp('^\\u' + u.toString(16).padStart(4, '0') + '$', 'i');
  for (const other of [ch.toUpperCase(), ch.toLowerCase()]) {
    if (other.length !== 1 || other === ch) continue;
    if (r.test(other) !== (canon(ch) === canon(other))) disagree++;
  }
}
console.log('i disagreements', disagree);

// Every BMP unit against its own one-unit upper and lower case, in both modes.
// The case pairs Unicode 16.0 added are left out, so the sums do not depend on
// which Unicode version an engine carries.
const added16 = new Set([0x019b, 0x0264, 0x1c89, 0x1c8a, 0xa7cb, 0xa7cc, 0xa7cd, 0xa7da, 0xa7db, 0xa7dc]);
function sweep(flags) {
  let sum = 0;
  let count = 0;
  for (let u = 0; u <= 0xffff; u++) {
    if (u >= 0xd800 && u <= 0xdfff) continue;
    if (added16.has(u)) continue;
    const hex = u.toString(16).padStart(4, '0');
    const r = new RegExp('^\\u' + hex + '$', flags);
    const ch = String.fromCharCode(u);
    const up = ch.toUpperCase();
    const lo = ch.toLowerCase();
    if (up.length === 1 && up !== ch && r.test(up)) {
      sum = (sum + (u + 1) * 3) % 2147483647;
      count++;
    }
    if (lo.length === 1 && lo !== ch && r.test(lo)) {
      sum = (sum + (u + 7) * 5) % 2147483647;
      count++;
    }
  }
  return count + ' ' + sum;
}
console.log('i', sweep('i'));
console.log('iu', sweep('iu'));

// A wide range under `i`: which ASCII letters and a few others each matches.
const wide = /^[Ā-￿]$/i;
let hits = '';
for (const c of 'AZaz@ſKıİµ') hits += wide.test(c) ? '1' : '0';
console.log(hits);

// toLowerCase / toUpperCase of strings that may already be in the asked case.
// Bronze answers an ASCII Latin-1 string with no letter to convert by
// returning it after a scan (builtin_string.cpp stringCaseImpl); every unit
// that defeats the scan has to be seen wherever it sits: first, at every
// offset an eight-unit-word scan would split at, in the tail, and
// next to the scan's bounds ('@' 0x40, '[' 0x5B, '`' 0x60, '{' 0x7B), plus
// Latin-1 units above 0x7F whose case maps (é, ÿ, µ, ß), two-byte strings, and
// strings still growing as builders.

const probes = ['A', 'Z', 'a', 'z', '@', '[', '`', '{', '0', '9', '-', 'é', 'É', 'ÿ', 'µ',
                'ß', '\u0080', ' ', 'İ', 'ı', '日'];
let lines = 0;
let digest = 0;
function mix(s) {
  for (let i = 0; i < s.length; i++) digest = (digest * 31 + s.charCodeAt(i)) | 0;
}
for (const fill of ['x', 'X', '5']) {
  for (let len = 0; len <= 20; len++) {
    const base = fill.repeat(len);
    const lo = base.toLowerCase();
    const up = base.toUpperCase();
    mix(lo); mix(up);
    for (let pos = 0; pos <= len; pos++) {
      for (const p of probes) {
        const s = base.slice(0, pos) + p + base.slice(pos);
        const l = s.toLowerCase();
        const u = s.toUpperCase();
        mix(l); mix(u);
        // ß uppercases to SS and İ lowercases to two code points.
        if (l.length !== s.length || u.length !== s.length) lines += l.length * 100 + u.length;
      }
    }
  }
}
console.log('digest', digest, 'length changes', lines);

// Spot values, printed whole.
const spots = [
  '', 'a', 'abcdefgh', 'abcdefghi', 'ABCDEFGH', 'abcdefgH', 'Habcdefg', 'abcdefghijklmnoP',
  '0123456789abcdef', '01234567-89ab-cdef-0123-456789abcdef', '01234567-89AB-cdef-0123-456789abcdef',
  '@@@@@@@@', '[[[[[[[[', '````````', '{{{{{{{{', 'abc@[`{xyz', 'ABC@[`{XYZ',
  'cafécafé', 'CAFÉCAFÉ', 'straße', 'ÿÿÿÿÿÿÿÿ',
  'µµ', 'abcİdef', 'ABCıDEF', '日本abc', '日本ABC',
];
for (const s of spots) {
  console.log(JSON.stringify(s), JSON.stringify(s.toLowerCase()), JSON.stringify(s.toUpperCase()),
              s.toLowerCase() === s, s.toUpperCase() === s, s.toLocaleLowerCase() === s.toLowerCase());
}

// Builders: a string grown by += is compared after each step.
let b = '';
const seen = [];
for (const ch of 'abcdefghijKlmnopqrst') {
  b += ch;
  seen.push(b.toLowerCase() === b ? 'l' : 'L', b.toUpperCase() === b ? 'u' : 'U');
}
console.log(seen.join(''));

// Two-byte strings whose units are all ASCII after a slice.
const wide = '日abcdefghijklmnop'.slice(1);
console.log(wide.toLowerCase() === wide, wide.toUpperCase(), ('日ABCDEFGHIJ').slice(1).toLowerCase());

// A String object goes the generic way. (A Number, Boolean or Array `this`
// is ToString'd by the spec, which bronze's String.prototype methods refuse
// with a TypeError today: reported in the ledger, left out of this case.)
console.log(String.prototype.toLowerCase.call(new String('ABCdefGHIJ')),
            String.prototype.toUpperCase.call(new String('abcdefghij')));
try {
  String.prototype.toLowerCase.call(null);
} catch (e) {
  console.log(e instanceof TypeError);
}

// A hot loop, so the tiers that compile late see the same answers.
let count = 0;
const pool = ['a1b2c3d4e5f6a7b8', 'A1b2c3d4e5f6a7b8', 'a1b2c3d4e5f6a7bZ', 'a1b2c3d4e5f6a7bé', 'abc'];
for (let i = 0; i < 5000; i++) {
  const s = pool[i % pool.length];
  if (s.toLowerCase() === s) count++;
  if (s.toUpperCase() === s) count += 1000;
}
console.log('loop', count);

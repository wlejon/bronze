// String.prototype.normalize (22.1.3.16): the four forms of UAX #15, printed
// as code points so the expectation reads as the standard does. The table and
// algorithm questions are asked again, unit by unit, in
// tests/runtime/unicode_norm_test.cpp; this case pins the member: its default
// form, its RangeError, its argument coercion and its answers end to end.
function cps(s) {
    return [...s].map(c => c.codePointAt(0).toString(16).toUpperCase().padStart(4, '0')).join(' ');
}
function show(label, s) {
    console.log(label + ': ' + cps(s));
}

// Decomposition of precomposed Latin letters, and back.
show('NFD e-acute', 'é'.normalize('NFD'));
show('NFD A-ring', 'Å'.normalize('NFD'));
show('NFD n-tilde', 'ñ'.normalize('NFD'));
show('NFD Creme Brulee', 'Crème Brûlée'.normalize('NFD'));
console.log('NFD length', 'Crème Brûlée'.normalize('NFD').length);
show('NFC e + acute', 'é'.normalize('NFC'));
console.log('NFC round trip', 'Crème Brûlée'.normalize('NFD').normalize('NFC') === 'Crème Brûlée');

// The default form is NFC, and undefined selects it too.
show('default', 'é'.normalize());
show('undefined', 'é'.normalize(undefined));

// Canonical ordering: dot below (220) sorts before dot above (230).
show('NFD a 0307 0323', 'ạ̇'.normalize('NFD'));
show('NFD a 0323 0307', 'ạ̇'.normalize('NFD'));
show('NFC a 0307 0323', 'ạ̇'.normalize('NFC'));
show('NFC 1E0B 0323', 'ḍ̇'.normalize('NFC'));

// Hangul, arithmetically.
show('NFD ga', '가'.normalize('NFD'));
show('NFC g a', '가'.normalize('NFC'));
show('NFD hih', '힣'.normalize('NFD'));
show('NFC hih', '힣'.normalize('NFC'));

// Compatibility forms.
show('NFKD fi', 'ﬁ'.normalize('NFKD'));
show('NFKC fi', 'ﬁ'.normalize('NFKC'));
show('NFC fi', 'ﬁ'.normalize('NFC'));
show('NFKC circled 1', '①'.normalize('NFKC'));
show('NFKC halfwidth ka', 'ｶ'.normalize('NFKC'));
show('NFKC halfwidth ga', 'ｶﾞ'.normalize('NFKC'));
show('NFKD nbsp', ' '.normalize('NFKD'));
show('NFKC math bold A', '\u{1D400}'.normalize('NFKC'));
console.log('NFKD FDFA length', 'ﷺ'.normalize('NFKD').length);

// Composition exclusions and singletons.
show('NFC 0958', 'क़'.normalize('NFC'));
show('NFC angstrom', 'Å'.normalize('NFC'));
show('NFD angstrom', 'Å'.normalize('NFD'));
show('NFC ohm', 'Ω'.normalize('NFC'));

// Astral: MUSICAL SYMBOL HALF NOTE is excluded from composition.
show('NFD 1D15E', '\u{1D15E}'.normalize('NFD'));
show('NFC 1D15E', '\u{1D15E}'.normalize('NFC'));

// An unpaired surrogate passes through, and blocks.
show('lone lead', '\uD800'.normalize('NFC'));
show('lone trail + e acute', '\uDC00é'.normalize('NFC'));
show('e lone acute', 'e\uD800́'.normalize('NFC'));

// The accent-folding idiom.
console.log('Crème'.normalize('NFD').replace(/\p{M}/gu, '') === 'Creme');
console.log('Crème Brûlée'.normalize('NFD').replace(/\p{M}/gu, ''));

// An already-normalized string answers equal to itself.
const plain = 'Hello, World!';
console.log(plain.normalize() === plain, plain.normalize('NFKD') === plain);
const composed = 'été';
console.log(composed.normalize('NFC') === composed, composed.normalize('NFD') === composed);

// The form is coerced with ToString, and anything else is a RangeError.
console.log(cps('é'.normalize({ toString() { return 'NFC'; } })));
for (const bad of ['bogus', 'nfc', 'NFC ', '', null]) {
    try {
        'a'.normalize(bad);
        console.log('no error for', String(bad));
    } catch (e) {
        console.log(String(bad), e instanceof RangeError);
    }
}
// A String object receiver normalizes its [[StringData]].
show('wrapper', String.prototype.normalize.call(new String('é')));

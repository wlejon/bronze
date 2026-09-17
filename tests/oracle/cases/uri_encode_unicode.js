// 19.2.6.5 Encode and 19.2.6.6 Decode work in code points: a character past
// U+007F is percent-escaped as the UTF-8 bytes of its code point, a surrogate
// pair as one four-byte sequence, and the decode reverses exactly that. bronze
// encoded from a byte view of the string that spelled every non-ASCII unit as
// 0xFF, so `encodeURIComponent('é')` was `%FF`.

console.log(encodeURIComponent('é'), encodeURIComponent('💩'), encodeURIComponent('日本語'));
console.log(encodeURI('http://x/a b?q=1&r=é#ü'), encodeURIComponent('http://x/a b?q=1&r=é#ü'));
console.log(encodeURIComponent("-_.!~*'()"), encodeURI(";,/?:@&=+$#"), encodeURIComponent(";,/?:@&=+$#"));
console.log(encodeURIComponent(''), encodeURIComponent('߿'), encodeURIComponent('ࠀ'), encodeURIComponent('￿'), encodeURIComponent('\u{10FFFF}'));

// The round trip, and a decode of escapes written by hand in either case.
for (const s of ['é', '💩', 'a b&c/é', '日本語 テスト', '\u{1F600}xéy']) {
    console.log(decodeURIComponent(encodeURIComponent(s)) === s, decodeURI(encodeURI(s)) === s);
}
console.log(decodeURIComponent('%E2%9C%93'), decodeURIComponent('%e2%9c%93'), decodeURIComponent('%F0%9F%92%A9').length);
// decodeURI keeps a reserved character's escape as written; the component
// form decodes it.
console.log(decodeURI('a%2Fb%3Fc%23d%20e'), decodeURIComponent('a%2Fb%3Fc%23d%20e'));
console.log(decodeURI('%C3%A9%2F'), decodeURIComponent('%C3%A9%2F'));

// URIError: a lone surrogate to encode; a truncated escape, a non-hex escape,
// a stray continuation byte, a lead byte short of its continuations, an
// overlong form, a surrogate spelled in UTF-8, and a value past U+10FFFF.
function probe(label, f) {
    try { console.log(label, JSON.stringify(f())); }
    catch (e) { console.log(label, e.name, e instanceof URIError); }
}
probe('lone high', () => encodeURIComponent('\uD800'));
probe('lone low', () => encodeURI('a\uDC00b'));
probe('pair ok', () => encodeURIComponent('😀'));
probe('truncated', () => decodeURIComponent('%E2%9C'));
probe('short', () => decodeURIComponent('%E'));
probe('nonhex', () => decodeURIComponent('%G0'));
probe('continuation', () => decodeURIComponent('%80'));
probe('lead', () => decodeURIComponent('%C3x'));
probe('overlong', () => decodeURIComponent('%C0%80'));
probe('surrogate', () => decodeURIComponent('%ED%A0%80'));
probe('past max', () => decodeURIComponent('%F4%90%80%80'));
probe('five byte', () => decodeURIComponent('%F8%88%80%80%80'));
probe('no arg', () => [encodeURIComponent(), decodeURI(), encodeURI(null), decodeURIComponent(12)]);

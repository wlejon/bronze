// The Annex B globals `escape` and `unescape` (ECMA-262 B.2.1.1 and B.2.1.2).
//
// They are normative for a web browser, which is what bronze compiles for, and
// they are NOT the URI pair: the alphabet is different, the escape for a
// non-Latin-1 character is `%uXXXX` rather than UTF-8 percent bytes, and
// malformed input to `unescape` is passed through instead of being a URIError.
//
// Derived from ECMA-262:
//
// 1. B.2.1.1 step 4 keeps 69 characters as themselves: the ASCII letters, the
//    digits, and `@*_+-./`. Everything else is escaped, which is why `~` is
//    escaped and `+` is not — no rule of thumb gets that pair right.
// 2. A code unit below 0x100 becomes `%XX` and anything else `%uXXXX`, both with
//    UPPERCASE hex digits (steps 5 and 6).
// 3. It works in CODE UNITS, so an astral character is TWO `%uXXXX` escapes —
//    its surrogates escaped separately — and a lone surrogate escapes cleanly.
// 4. B.2.1.2 recognises `%uXXXX` first and `%XX` second, accepts hex digits in
//    either case, and PASSES THROUGH any `%` that does not begin a well-formed
//    escape (steps 5.b and 5.c fall through). So `unescape("%")` is "%" and
//    `unescape("%zz")` is "%zz", where `decodeURI` of either is a URIError.
// 5. The `u` is lowercase only: `%U0041` is not an escape.
// 6. Step 1 is ToString, so a non-string argument is converted and an absent one
//    is "undefined".
// 7. `unescape(escape(s))` is `s` for every string, which is the pair's contract.
console.log(typeof escape, typeof unescape);

// 1 & 2: the alphabet.
console.log(escape('abcXYZ019'));
console.log(escape('@*_+-./'));
console.log(escape('~'));
console.log(escape('a b'));
console.log(escape('a~b'));
console.log(escape('!"#$%&\'()'));
console.log(escape(':;<=>?'));
console.log(escape('[\\]^`{|}'));
console.log(escape(''));
console.log(escape('').length);

// 2: uppercase hex, and the two widths.
console.log(escape(String.fromCharCode(0x00)));
console.log(escape(String.fromCharCode(0x0a)));
console.log(escape(String.fromCharCode(0xff)));
console.log(escape(String.fromCharCode(0x100)));
console.log(escape(String.fromCharCode(0x1234)));
console.log(escape(String.fromCharCode(0xabcd)));

// 3: code units, not code points.
console.log(escape(String.fromCodePoint(0x1f600)));
console.log(escape(String.fromCharCode(0xd800)));

// 4: unescape, and its pass-through.
console.log(unescape('a%20b%7Ec'));
console.log(unescape('%u1234') === String.fromCharCode(0x1234));
console.log(unescape('%u1234').length);
console.log(unescape('%7e'), unescape('%7E'));
console.log(unescape('%u00ff') === String.fromCharCode(0xff));
console.log(unescape('%'));
console.log(unescape('%z'));
console.log(unescape('%zz'));
console.log(unescape('%2'));
console.log(unescape('%u12'));
console.log(unescape('%u123z'));
console.log(unescape('100%'));
console.log(unescape('plain'));
console.log(unescape('').length);

// 5: the lowercase `u`.
console.log(unescape('%U0041'));
console.log(unescape('%u0041'));

// 6: ToString on the argument.
console.log(escape(123));
console.log(escape(undefined));
console.log(escape(null));
console.log(escape(true));
console.log(unescape(4.5));

// 7: the round trip.
const samples = ['', 'plain', 'a b~c', 'ümlaut', String.fromCodePoint(0x1f600),
  '!"#$%&\'()*+,-./:;<=>?@[\\]^_`{|}~', String.fromCharCode(0, 1, 0xff, 0x100, 0xffff)];
console.log(samples.every((s) => unescape(escape(s)) === s));
console.log(samples.map((s) => escape(s).length).join(','));

// They are properties of the global object, and the same objects through it.
console.log(globalThis.escape === escape, globalThis.unescape === unescape);

// `RegExp.escape` (ECMA-262 22.2.5.2) — the text a pattern would need in order
// to match its argument LITERALLY.
//
// Derived from ECMA-262:
//
// 1. Step 1: a non-String argument is a TypeError. It is not ToString'd, which
//    is the member refusing to guess: the whole value of it is that the result
//    is safe to splice into a pattern, and a number quietly stringified is how
//    a caller escapes the wrong thing.
// 2. Step 3.a: the FIRST code point takes a `\xHH` escape when it is a decimal
//    digit or an ASCII letter, so the result can never read as a flag or as part
//    of an identifier at the splice point. Only the first — `escape("ab")` is
//    "\x61b".
// 3. 22.2.5.2.1 EncodeForRegExpEscape, in three tiers:
//    a. a SyntaxCharacter (^ $ \ . * + ? ( ) [ ] { } |) or `/` takes a
//       backslash;
//    b. the five control characters take their named escape (\t \n \v \f \r);
//    c. anything in ,-=<>#&!%:;@~'`" or matched by WhiteSpace or
//       LineTerminator, or a lone surrogate, takes `\xHH` when it fits in a byte
//       and `\u{...}` when it does not.
// 4. Everything else — a letter that is not first, a non-ASCII letter, an astral
//    character — is itself. StringToCodePoints pairs surrogates first, so an
//    astral character is ONE code point and does not reach the lone-surrogate
//    rule.
// 5. The result really does match: `new RegExp(RegExp.escape(s))` matches `s`
//    and nothing it did not mean to.
console.log(typeof RegExp.escape);

// 2: the first code point.
console.log(RegExp.escape('ab'));
console.log(RegExp.escape('1abc'));
console.log(RegExp.escape('Zed'));
console.log(RegExp.escape('_ab'));
console.log(RegExp.escape(''));
console.log(RegExp.escape('').length);

// 3.a: the syntax characters and the solidus.
console.log(RegExp.escape('.'));
console.log(RegExp.escape('^$\\.*+?()[]{}|'));
console.log(RegExp.escape('/'));
console.log(RegExp.escape('a.b*c'));

// 3.b: the control escapes.
console.log(RegExp.escape('\t\n\v\f\r'));

// 3.c: punctuators, whitespace and a lone surrogate.
console.log(RegExp.escape(',-=<>#&!%:;@~\'`"'));
console.log(RegExp.escape('hello world'));
console.log(RegExp.escape('a' + String.fromCharCode(0xa0) + 'b'));
console.log(RegExp.escape('a' + String.fromCharCode(0x2028) + 'b'));
console.log(RegExp.escape('a' + String.fromCharCode(0x3000) + 'b'));
console.log(RegExp.escape(String.fromCharCode(0xd800)));
console.log(RegExp.escape('a' + String.fromCharCode(0xdfff) + 'b'));

// 4: what is left alone.
console.log(RegExp.escape('_x-y'));
console.log(RegExp.escape('x' + String.fromCharCode(0xe9)));
console.log(RegExp.escape('x' + String.fromCodePoint(0x1f600)));
console.log(RegExp.escape('x' + String.fromCodePoint(0x1f600)).length);

// 5: the round trip.
const tricky = 'a.b*c(d)[e]{f}|g^h$i/j k';
const re = new RegExp(RegExp.escape(tricky));
console.log(re.test(tricky));
console.log(re.test('aXbXc(d)[e]{f}|g^h$i/j k'));
console.log(new RegExp(RegExp.escape('a.c')).test('abc'));
console.log(new RegExp(RegExp.escape('a.c')).test('a.c'));

// 1: the refusals.
function reason(fn) {
  try {
    return fn();
  } catch (e) {
    return e.name;
  }
}
console.log(reason(() => RegExp.escape(5)));
console.log(reason(() => RegExp.escape(undefined)));
console.log(reason(() => RegExp.escape(null)));
console.log(reason(() => RegExp.escape(['a'])));
console.log(reason(() => RegExp.escape()));

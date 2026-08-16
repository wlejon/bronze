// 22.1.3.28 toUpperCase and 22.1.3.30 toLowerCase, which are 11.1.3 over
// Unicode Default Case Conversion (UCD 3.13). FULL mapping, not simple: the
// result may be a different LENGTH from the input, and the two directions are
// not inverses of each other.
//
// Every character is written as an escape and every answer is pinned as a list
// of code points, because that is the only expectation that cannot be read two
// ways: a file holding the characters themselves would pin its own encoding as
// much as the mapping.
//
// The three facts that separate full mapping from a 26-letter loop:
//
//   U+00DF ß uppercases to TWO code points, "SS" — and lowercasing that back
//     gives "ss", so the round trip is lossy in one direction only. U+1E9E ẞ,
//     the capital that was added later, lowercases to ß, so ß has an uppercase
//     it does not come back from.
//   U+0130 İ lowercases to TWO code points, i + U+0307 COMBINING DOT ABOVE.
//     The Turkish tailoring that would answer a bare "i" is a locale rule
//     (SpecialCasing.txt marks it `tr`), which default casing excludes.
//   U+03A3 Σ lowercases to U+03C3 σ or U+03C2 ς depending on CONTEXT — the
//     Final_Sigma condition, the one language-independent context rule in the
//     whole of default casing. "ΣΑΣ" is "σας": the first sigma has no cased
//     letter before it, the last has no cased letter after it.
//
// Astral mappings go through the surrogate pair (Deseret), and the Cherokee
// block is the one whose UPPERCASE letters sort below its lowercase ones,
// which a table built from a range assumption would get backwards.
//
// The ASCII lines come first and are the invariant: whatever the tables say,
// the 26 letters answer exactly what they answered before there were any.
function cps(s) {
  const out = [];
  for (let i = 0; i < s.length; i++) {
    out.push('U+' + s.charCodeAt(i).toString(16).toUpperCase());
  }
  return out.join(' ');
}

console.log('Hello, World'.toUpperCase(), 'Hello, World'.toLowerCase());
console.log('abcXYZ 123 !@#'.toUpperCase());
console.log('abcXYZ 123 !@#'.toLowerCase());
console.log(''.toUpperCase() === '', ''.toLowerCase() === '');

// U+00DF / U+1E9E, and the asymmetry.
console.log('ß'.toUpperCase(), 'ß'.toUpperCase().length);
console.log(cps('ß'.toUpperCase()));
console.log(cps('ß'.toUpperCase().toLowerCase()));
console.log(cps('ẞ'.toLowerCase()));
console.log('ß'.toUpperCase().toLowerCase() === 'ß');
console.log(cps('straße'.toUpperCase()));

// U+0130, two code points down.
console.log(cps('İ'.toLowerCase()), 'İ'.toLowerCase().length);
console.log(cps('İ'.toUpperCase()));
console.log(cps('ı'.toUpperCase()));

// Final_Sigma.
console.log(cps('ΣΑΣ'.toLowerCase()));
console.log(cps('Σ'.toLowerCase()));
console.log(cps('ΣΣ'.toLowerCase()));
console.log(cps('ς'.toUpperCase()), cps('σ'.toUpperCase()));
// A case-ignorable character does not break the run: the sigma before the
// combining acute is still final.
console.log(cps('ΑΣ.'.toLowerCase()));

// Latin-1 and the rest of the BMP.
console.log(cps('é'.toUpperCase()), cps('É'.toLowerCase()));
console.log(cps('µ'.toUpperCase()));
console.log(cps('ﬁ'.toUpperCase()));
console.log(cps('ŉ'.toUpperCase()));

// Astral: Deseret, through the surrogate pair.
console.log(cps('𐐀'.toLowerCase()));
console.log(cps('𐐨'.toUpperCase()));
console.log('𐐀'.toLowerCase().length);

// Cherokee.
console.log(cps('Ꭰ'.toLowerCase()), cps('ꭰ'.toUpperCase()));

// A lone surrogate is not a code point pair and is left alone.
console.log(cps('\uD800'.toUpperCase()), cps('\uDFFF'.toLowerCase()));

// The toLocale* twins share the default mapping, because bronze carries no
// tailoring: the Turkish rule that would make this U+0130 is not here.
console.log(cps('i'.toLocaleUpperCase()), cps('I'.toLocaleLowerCase()));
console.log('ß'.toLocaleUpperCase() === 'ß'.toUpperCase());
console.log('ΣΑΣ'.toLocaleLowerCase() === 'ΣΑΣ'.toLowerCase());

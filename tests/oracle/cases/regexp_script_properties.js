// `\p{Script=...}` and `\p{Script_Extensions=...}` (22.2.1's
// UnicodePropertyValueExpression over UAX #24), and the difference between
// them, which is the whole reason there are two properties.
//
// Script is a PARTITION: every code point has exactly one, and a code point
// Scripts.txt does not name has `Unknown` — a real value, not a hole, which is
// why `\p{Script=Unknown}` matches an unassigned character rather than nothing.
// Script_Extensions is a SET per code point, and it differs from Script only
// for characters COMMONLY USED with a script they do not belong to. U+0342
// COMBINING GREEK PERISPOMENI is the clean case: Inherited by Script, Greek by
// Script_Extensions, so a pattern meaning "Greek text" wants the second and a
// pattern meaning "letters of the Greek alphabet" wants the first. U+3099 is
// the same shape with a set of two — Inherited by Script, Hiragana AND
// Katakana by Script_Extensions.
//
// Both spellings of each property name the same set (22.2.1 lists the alias
// beside the canonical name), and so do both spellings of a VALUE: `Grek` is
// `Greek` and `Qaac` is `Coptic`, because PropertyValueAliases.txt says so.
// Matching is exact and case sensitive, so `grek` is a syntax error and not a
// sloppy `Grek` — the same rule `\p{lu}` already lives under.
//
// The lone form is deliberately not a Script lookup: 22.2.1 reads `\p{Greek}`
// as a General_Category value or a binary property name, and there is no
// category called Greek, so it is a syntax error saying which spelling would
// have worked.

function message(f) {
  try {
    f();
    return "no error";
  } catch (e) {
    return e instanceof SyntaxError ? e.message : "wrong error kind";
  }
}

function names(f, text) {
  return message(f).indexOf(text) >= 0;
}

const alpha = "α";        // GREEK SMALL LETTER ALPHA
const perispomeni = "͂";  // COMBINING GREEK PERISPOMENI
const voiced = "゙";       // COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK

// A letter of the script, by either spelling of the property and of the value.
console.log(/\p{Script=Greek}/u.test(alpha), /\p{sc=Grek}/u.test(alpha));

// The disagreement the two properties exist for.
console.log(/\p{Script=Greek}/u.test(perispomeni),
            /\p{Script_Extensions=Greek}/u.test(perispomeni));
console.log(/\p{scx=Grek}/u.test(perispomeni), /\p{Script=Inherited}/u.test(perispomeni));

// A Script_Extensions set with two members in it.
console.log(/\p{Script=Hiragana}/u.test(voiced), /\p{scx=Hiragana}/u.test(voiced),
            /\p{scx=Katakana}/u.test(voiced));

console.log(/\p{Script=Han}/u.test("漢"), /\p{Script=Latin}/u.test("A"),
            /\p{Script=Latin}/u.test(alpha));

// `Qaac` is a third alias for Coptic, and 22.2.1 takes every alias the UCD
// lists rather than the two-name subset a table written by hand would carry.
console.log(/\p{Script=Coptic}/u.test("Ⲁ"), /\p{sc=Qaac}/u.test("Ⲁ"));

// A script above the BMP is one code point under `u`, so the anchors hold.
console.log(/^\p{Script=Deseret}$/u.test("\u{10400}"));

// `\P` complements over the CODE POINT ceiling, so it is the whole rest of the
// alphabet and not the rest of the BMP.
console.log(/\P{Script=Latin}/u.test("A"), /\P{Script=Latin}/u.test(alpha),
            /^\P{Script=Latin}$/u.test("\u{10400}"));

// Common and Unknown are Script values like any other.
console.log(/\p{Script=Common}/u.test("!"), /\p{Script=Unknown}/u.test("͸"));

// A value that names no script is a syntax error naming the value, and the
// case-sensitivity rule makes `grek` exactly that rather than a `Grek`.
console.log(names(function () { return new RegExp("\\p{Script=Greeek}", "u"); }, "Greeek"),
            names(function () { return new RegExp("\\p{sc=grek}", "u"); }, "grek"));

// The lone form names a category or a binary property, never a script.
console.log(names(function () { return new RegExp("\\p{Greek}", "u"); }, "lone"),
            names(function () { return new RegExp("\\p{Greek}", "u"); }, "Script=Greek"));

// And the escape is still a +UnicodeMode production, script or not.
console.log(names(function () { return new RegExp("\\p{Script=Greek}"); }, "`u` flag"));

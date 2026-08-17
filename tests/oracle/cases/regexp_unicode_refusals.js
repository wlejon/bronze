// What the `u` flag takes AWAY, and what it hands back in the same breath.
//
// Two different things, pinned together because a reader who finds one will ask
// about the other.
//
// The first is 22.2.1's own doing. Annex B's B.1.2 extensions are `~UnicodeMode`
// productions: `{` as an ordinary character, a lone `]` or `}`, an identity
// escape before anything at all. Under `u` the grammar has no production for
// them, so each becomes a syntax error where it was legal a flag ago. bronze
// diagnoses each by name at the literal rather than reinterpreting it, which is
// the hard-error rule and also the only way a program can tell which reading it
// got.
//
// The second is what the same flag switches ON, which is the half a reader
// stops looking for once the first half has convinced them `u` only forbids
// things. `\p{...}` is a +UnicodeMode production too, so it is legal ONLY with
// the flag — and it reads a real General_Category table, because 22.2.1's
// UnicodePropertyValueExpression is defined by reference to UAX #44 and
// nothing less answers it. `u` together with `i` is legal as well, and means
// 22.2.2.9 step 1: simple case folding rather than the uppercase mapping,
// which is a second table and not a relaxed reading of the first (U+017F folds
// to `s` under one and to itself under the other).
//
// Both appear below only as the answer to "and what does it enable?".
// regexp_unicode_property and regexp_unicode_property_refusals are what the
// properties mean; regexp_unicode_case_folding is what the fold means.

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

// Annex B B.1.2, switched off. Each pair is the same pattern text: legal
// without `u`, a named syntax error with it.
console.log(new RegExp("a{").test("a{"),
            names(function () { return new RegExp("a{", "u"); }, "does not begin a quantifier"));
// An INCOMPLETE quantifier is the same refusal, reached after the quantifier
// reader has declined the `{` and left it to the atom position.
console.log(names(function () { return new RegExp("a{2", "u"); }, "does not begin a quantifier"));
console.log(new RegExp("]").test("]"),
            names(function () { return new RegExp("]", "u"); }, "a lone `]`"));
console.log(new RegExp("}").test("}"),
            names(function () { return new RegExp("}", "u"); }, "a lone `}`"));
console.log(new RegExp("\\-").test("-"),
            names(function () { return new RegExp("\\-", "u"); },
                  "not a valid escape under the `u` flag"));

// What +UnicodeMode's IdentityEscape DOES take — a SyntaxCharacter or `/` — and
// the `-` that ClassEscape adds inside a class, so `[\-]` stays writable.
console.log(new RegExp("\\]\\}\\{\\$\\/", "u").test("]}{$/"),
            new RegExp("[\\-]", "u").test("-"));
// A valid quantifier is still a quantifier, and a class is still a class.
console.log(new RegExp("a{2,3}", "u").test("aa"), new RegExp("[\\]]", "u").test("]"));

// `\u{...}` is a code point escape only under `u`. Without it the sequence is
// Annex B's quantified `\u`, which bronze has never implemented and still
// refuses by name rather than quietly reading it one way or the other.
console.log(names(function () { return new RegExp("\\u{2}"); }, "only under the `u` flag"));
console.log(names(function () { return new RegExp("\\u{}", "u"); },
                  "at least one hexadecimal digit"));
console.log(names(function () { return new RegExp("\\u{110000}", "u"); }, "above U+10FFFF"));

// `\p{...}`: a set with the flag, and still a named refusal without it, since
// the production does not exist there and Annex B would read it as the letter
// `p`.
console.log(new RegExp("\\p{L}", "u").test("a"), new RegExp("\\P{L}", "u").test("1"));
console.log(names(function () { return new RegExp("\\P{L}"); }, "unicode property escapes"));

// `u` with `i`: a COMBINATION that compiles, and whose whole effect is which
// table 22.2.2.9 reads. The second line is the difference in one character —
// `/ſ/i` does not match "s" and `/ſ/iu` does.
console.log(new RegExp("a", "ui").test("A"));
console.log(new RegExp("\\u017f", "iu").test("s"));
console.log(message(function () { return new RegExp("a", "u"); }),
            message(function () { return new RegExp("a", "i"); }));

// `v` is the same story one edition later. It takes the class grammar away —
// `[(]` is a set expression's reserved punctuation there, not the parenthesis —
// and hands back set operations, and members that are strings, for it. The
// refusal left inside that feature is about its SPELLING: `\q` without braces
// is not a ClassStringDisjunction, and the diagnostic names the production
// rather than the two characters.
//
// The second is the flag pair 22.2.3.4 forbids. Both letters are legal on their
// own and neither is a superset of the other's spelling, so the only place the
// conflict can be reported is the parse, and bronze reports it by naming both.
console.log(names(function () { return new RegExp("[\\qab]", "v"); },
                  "ClassStringDisjunction"),
            names(function () { return new RegExp("a", "uv"); },
                  "`u` and `v` cannot both be set"));

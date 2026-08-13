// What the `u` flag takes AWAY, and what bronze still refuses with it.
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
// The second is bronze's. `\p{...}` is legal JavaScript under `u` and is still
// refused here: 22.2.1's UnicodePropertyValueExpression is defined by reference
// to UAX #44, and bronze carries no General Category or Script table. The
// refusal names that, not the flag — reading `\p{L}` as the letter `p` is
// exactly the silent wrong answer it exists to prevent. And `u` together with
// `i` is refused as a COMBINATION: 22.2.2.9 Canonicalize switches to simple
// case folding under both, a different table from the uppercase mapping bronze
// carries for `i` alone (U+017F folds to `s` under one and to itself under the
// other), so reusing it would be wrong in a way only a test spelling one of
// those characters could catch.

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

// `\p{...}`: refused with `u` as well as without, and for the reason that
// survives the flag.
console.log(names(function () { return new RegExp("\\p{L}", "u"); }, "unicode property escapes"),
            names(function () { return new RegExp("\\p{L}", "u"); }, "UAX #44"));
console.log(names(function () { return new RegExp("\\P{L}"); }, "unicode property escapes"));

// `u` with `i`: a refusal on the COMBINATION, which removes nothing, since `u`
// was refused outright until now. Each flag on its own still compiles.
console.log(names(function () { return new RegExp("a", "ui"); }, "`u` and `i` flags together"));
console.log(names(function () { return new RegExp("a", "iu"); }, "simple case folding"));
console.log(message(function () { return new RegExp("a", "u"); }),
            message(function () { return new RegExp("a", "i"); }));

// The flags bronze still does not implement at all, unchanged.
console.log(names(function () { return new RegExp("a", "v"); }, "`v` flag"),
            names(function () { return new RegExp("a", "d"); }, "match indices"));

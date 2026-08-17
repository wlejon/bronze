// The `v` flag: a character class that is a set EXPRESSION.
//
// 22.2.1 gives a class two grammars and one flag chooses between them. Without
// `v` a class is ClassRanges — a flat list of members where the only structure
// is `a-z`. With `v` it is ClassSetExpression: classes nest, `--` takes a
// difference, `&&` an intersection, and the result is a set computed at compile
// time rather than a list walked at match time.
//
// The half a reader notices second is that the same flag REFUSES more than it
// used to. Every character that could begin an operator a later edition adds —
// `( ) [ ] { } / - | ` and every doubled punctuator — is a syntax error written
// bare, where in every other mode it is simply the character. That is the price
// of the first half and not an inconvenience of it: a pattern spelled for
// tomorrow's operator must not quietly match today. The last lines pin the
// refusals for that reason, and the very last line pins that the same pattern
// TEXT still means the old thing without the flag.
//
// What `v` does to case-insensitive matching is a different question with its
// own case: regexp_v_flag_case. What it does not implement — `\q{...}` and the
// properties of strings, which are one feature — is named here and in
// regexp_unicode_refusals.

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

// Union is still juxtaposition; what is new is that a member of it may be a
// whole class.
console.log("abcwxyz".replace(/[[a-c][x-z]]/gv, "."));

// ClassSubtraction and ClassIntersection. Neither takes a bare range as an
// operand — a range is not a ClassSetOperand — so both sides are nested
// classes, which is the shape the grammar forces and not a style choice.
console.log("abcdefghij".replace(/[[a-j]--[aeiou]]/gv, "."));
console.log("0123456789abcdef".replace(/[[0-9a-f]&&[a-z]]/gv, "*"));

// Both are left-associative chains rather than two-operand productions, so
// three operands need no parentheses and mean what reading left to right says.
console.log("abcdef".replace(/[[a-f]--[a]--[b]]/gv, "."),
            "abcdef".replace(/[[a-f]&&[b-f]&&[c-f]]/gv, "."));

// A complement is an operand like any other, so an intersection can subtract
// with one. Under `v` a `[^...]` complements the SET at compile time instead of
// inverting the answer at match time, which is what lets it compose at all.
console.log(/[[a-z]&&[^aeiou]]/v.test("b"), /[[a-z]&&[^aeiou]]/v.test("e"));

// And an operand may itself be an operation, to any depth.
console.log("abcdef".replace(/[[[a-f]--[b]]&&[a-c]]/gv, "."));

// The flag reads back as `unicodeSets`, and it is NOT `unicode`: 22.2.6 gives
// them separate accessors because they are separate modes.
console.log(/a/v.flags, /a/v.unicodeSets, /a/v.unicode, /a/u.unicodeSets,
            /a/.unicodeSets);

// `v` implies everything +UnicodeMode gives, without the letter `u`: an astral
// range is one interval and `.` is one code point, not a surrogate half.
console.log(/^[\u{1F600}-\u{1F64F}]$/v.test("\u{1F60A}"), /^.$/v.test("\u{1F600}"));

// Property escapes are +UnicodeMode productions too, so they are legal here for
// the same reason.
console.log(/^\p{Script=Greek}+$/v.test("αβγ"), /^\p{L}$/v.test("a"));

// The reserved characters. A bare `(`, a trailing `-`, a doubled punctuator:
// each is an ordinary member elsewhere and a named syntax error here.
console.log(names(function () { return new RegExp("[(]", "v"); }, "must be escaped"),
            names(function () { return new RegExp("[a-]", "v"); }, "must be escaped"),
            names(function () { return new RegExp("[!!]", "v"); },
                  "reserved double punctuator"));

// `--` and `&&` have separate productions and no precedence between them, so a
// class that uses both has no parse. bronze says which two operators, and what
// to write instead, rather than picking an order.
console.log(names(function () { return new RegExp("[[a]--[b]&&[c]]", "v"); },
                  "cannot be mixed"));

// 22.2.3.4 ParsePattern step 1: `u` and `v` together, in either order.
console.log(names(function () { return new RegExp("a", "uv"); },
                  "`u` and `v` cannot both be set"),
            names(function () { return new RegExp("a", "vu"); },
                  "`u` and `v` cannot both be set"));

// `\q{...}` itself is implemented and `regexp_v_strings.js` pins what it
// matches; what this file keeps pinned is the two ways it is REFUSED. Its
// braces are not optional, and a class that may contain strings cannot be
// negated — 22.2.1's one early error about the feature, since the complement of
// a set of sequences is not a set of characters.
console.log(names(function () { return new RegExp("[\\qab]", "v"); },
                  "ClassStringDisjunction"),
            names(function () { return new RegExp("[^\\q{ab}]", "v"); },
                  "negated"));

// The other half of 22.2.1's string-capable sets, still refused and named as
// what it is. Table 67 is a closed list of seven, so this refusal can say
// "property of strings" where an unknown binary property can only be told it
// might be a misspelling — and it names `\q{...}` as the thing that DOES work,
// because what these seven lack is Unicode's sequence data and not a class that
// can hold a member longer than one character.
console.log(names(function () { return new RegExp("\\p{RGI_Emoji}", "v"); },
                  "property of STRINGS"),
            names(function () { return new RegExp("[\\p{Basic_Emoji}]", "v"); },
                  "\\q{...}"));

// Escaped, every reserved character is itself again; and the punctuators that
// are only reserved DOUBLED are ordinary written once.
console.log(/^[\(\|\-]+$/v.test("(|-"), /^[&!~]+$/v.test("&!~"));

// `[]` is the empty set — it matches nothing — and `[^]` its complement.
console.log(/[]/v.test("a"), /^[^]$/v.test("a"));

// The same pattern TEXT, without the flag, is still the old grammar: `[(]` is
// the parenthesis, and `[[a-c][x-z]]` is a class, a class and a literal `]`.
console.log(/^[(]$/.test("("), /^[[a-c][x-z]]$/.test("ax]"));

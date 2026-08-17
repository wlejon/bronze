// The `v` flag's class members that are STRINGS: 22.2.1's ClassStringDisjunction
// (`\q{...}`), and 22.2.2.9's set algebra over classes that hold one.
//
// The `v` flag changes what a class CONTAINS: not a set of code points but a set
// of strings, most of which happen to be one code point long. `\q{abc|d}` is the
// literal spelling of that — a class member three characters wide — and once a
// class can hold one, `--` (difference) and `&&` (intersection) are operations
// over sets whose members may be multi-character.
//
// Matching is longest-first among the members (22.2.2.9.6), which is why
// `[\q{abc|a}]` on "abc" consumes all three characters rather than one; and each
// member is a whole alternative, given back when what follows it fails.
//
// The other half of 22.2.1's string-capable sets — the properties of strings,
// `\p{RGI_Emoji}` and its six siblings — is still refused by name, for a reason
// that is now data rather than representation: each of the seven is a list of
// emoji sequences from UTS #51. `blocked/regexp_v_properties_of_strings.js` pins
// what a conforming engine answers for those.

const strings = /[\q{abc|d}]/v;
console.log(strings.test("abc"), strings.test("d"), strings.test("a"));
console.log("xabcy".replace(strings, "-"));

// Longest alternative first: the class holds both "abc" and "a".
console.log("abc".match(/[\q{abc|a}]/v)[0]);

// The empty string is a legal member, and it matches at any position.
console.log(/^[\q{|x}]$/v.test(""), /^[\q{|x}]$/v.test("x"));

// Difference and intersection. Both take ClassSetOperands — a nested class, a
// class escape, a `\q{...}`, or a single character — and a RANGE is not one, so
// the left side here is `[a-f]` nested and not a bare `a-f`. 22.2.1 gives
// `ClassSubtraction :: ClassSetOperand -- ClassSetOperand` and nothing else, so
// `[a-f--[b-d]]` has no parse at all.
console.log("abcdefgh".replace(/[[a-f]--[b-d]]/gv, "."));
console.log("a1B2".replace(/[\p{ASCII}&&\p{Letter}]/gv, "."));

// A string set inside a difference: "ab" is removed, the single letters stay.
console.log("ab a b".replace(/[\q{ab|a|b}--\q{ab}]/gv, "."));

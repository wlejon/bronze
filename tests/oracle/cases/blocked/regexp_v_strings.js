// BLOCKED: `invalid regular expression: unsupported: `\q{...}` is a set of
// STRINGS (22.2.1's ClassStringDisjunction), and bronze implements neither it
// nor the properties of strings it shares a representation with — a CharSet
// whose members are not single characters`.
//
// The `v` flag (22.2.2.9 CompileToCharSet with UnicodeSetsMode) changes what a
// class CONTAINS: not a set of code points but a set of STRINGS, most of which
// happen to be one code point long. `\q{abc|d}` is the literal spelling of
// that — a class member that is three characters — and once a class can hold
// one, the two set operators become meaningful: `--` is difference and `&&` is
// intersection, both of them over sets whose members may be multi-character.
//
// Matching is longest-first among the strings of a class (22.2.2.9.6), which is
// why `[\q{abc|a}]` on "abc" consumes all three characters rather than one.
//
// Unblocking this means a CharSet whose members are strings rather than code
// points, which is the representation the properties of strings
// (`\p{RGI_Emoji}` and its siblings, also unimplemented) need as well.

const strings = /[\q{abc|d}]/v;
console.log(strings.test("abc"), strings.test("d"), strings.test("a"));
console.log("xabcy".replace(strings, "-"));

// Longest alternative first: the class holds both "abc" and "a".
console.log("abc".match(/[\q{abc|a}]/v)[0]);

// The empty string is a legal member, and it matches at any position.
console.log(/^[\q{|x}]$/v.test(""), /^[\q{|x}]$/v.test("x"));

// Difference and intersection.
console.log("abcdefgh".replace(/[a-f--[b-d]]/gv, "."));
console.log("a1B2".replace(/[\p{ASCII}&&\p{Letter}]/gv, "."));

// A string set inside a difference: "ab" is removed, the single letters stay.
console.log("ab a b".replace(/[\q{ab|a|b}--\q{ab}]/gv, "."));

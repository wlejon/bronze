// BLOCKED: `unsupported: indexing a string is not implemented (bronze has no
// String exotic object; use s.charAt(i) or s.codePointAt(i))`.
//
// `"abc"[0]` used to answer `undefined`, silently, because the string branch of
// the property path tried `length`, then a method, then the unimplemented-member
// table, and fell off its end. Its sibling `"abc"[i]` with a variable index was
// already a hard error, so one operation had two answers and the wrong one was
// the quiet one. It is now refused by name in both spellings; this case is what
// forces the refusal to be replaced by the behaviour rather than left standing.
//
// What it needs is the String exotic object (10.4.3): a receiver whose
// [[GetOwnProperty]] consults StringGetOwnProperty first, so that a canonical
// numeric string below the length is a single-code-unit String value and
// anything else falls through to the ordinary lookup. That is the same missing
// piece as `cases/blocked/object_intrinsic_prototypes` — bronze hands string
// methods out beside the value instead of finding them on a prototype — so the
// two land together or not at all.
//
// What this pins when it lands, from 10.4.3.5 (StringGetOwnProperty) and
// 6.1.4 (a String value is a sequence of UTF-16 code units):
//
// 1. An in-range index is a String of length 1, not a character code.
// 2. An out-of-range index is `undefined` — there is no prototype chain step
//    that could answer it, and it is not an error.
// 3. A computed index answers exactly as a written one does.
// 4. Indexing composes: the result is an ordinary string value.
const s = "abc";
console.log(s[0], s[1], s[2]);
console.log(s[3]);
console.log(typeof s[0], s[0].length);

const i = 1;
console.log(s[i]);

console.log("hello"[0] + "hello"[4]);
console.log(s[0] === "a", s[0] === 97);

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
// anything else falls through to the ordinary lookup. "Falls through to" is
// the part bronze does not have: string methods are handed out BESIDE the value
// by the property path, so there is no ordinary lookup to fall through to.
// `cases/object_intrinsic_prototypes` is that same change made for `Object`,
// and it is the shape this one wants — a real prototype object on a real chain
// — one level down, where the receiver is a primitive rather than an object.
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

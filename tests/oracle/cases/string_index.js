// Indexing a string, in both spellings, and the mechanism that made one answer
// possible for the two of them.
//
// A String value is a sequence of UTF-16 code units (6.1.4), and 10.4.3.5
// StringGetOwnProperty is what turns a canonical numeric string below the length
// into ONE of them. The part bronze did not have was not that function: it was
// the FALL-THROUGH. 10.4.3 consults StringGetOwnProperty first and then the
// ordinary lookup, and a string's members used to be handed out BESIDE the
// value by the property path, so there was no ordinary lookup for a miss to
// fall through to. `String.prototype` is a real object on the real chain now
// (`cases/primitive_wrapper_objects` is its other half), which is what gives an
// index somewhere to miss into.
//
// The two spellings are the reason this case is not folded into
// `string_methods`. `"abc"[0]` took the by-name property path and `"abc"[i]`
// took the computed one, and the two disagreed — one read `undefined` and the
// other was a hard error. Both now reach the same function, and the last four
// lines are what pins that.
//
// What this pins, from 10.4.3.5 and 6.1.4:
//
// 1. An in-range index is a String of length 1, not a character code.
// 2. An out-of-range index is `undefined` — nothing on the prototype chain
//    answers a numeric name, and it is not an error.
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

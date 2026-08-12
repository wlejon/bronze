// String.prototype.charCodeAt. The index goes through ToIntegerOrInfinity,
// so a fraction truncates and a non-number coerces; an index outside the
// string is NaN, which is the answer that matters most — 0 is a real code
// unit (NUL), so returning it for a missing position is indistinguishable
// from finding one.
//
// charCodeAt reads UNITS, not code points: an astral character answers with its
// high and low surrogate separately, which is exactly the distinction for-of
// does not make. Every expectation is ECMA-262, derived by hand.
const s = "abc";
console.log(s.charCodeAt(0));
console.log(s.charCodeAt(2));
console.log(s.charCodeAt());
console.log(s.charCodeAt(3));
console.log(s.charCodeAt(-1));
console.log(s.charCodeAt(1.7));
console.log(s.charCodeAt("1"));
console.log(s.charCodeAt(true));
console.log(s.charCodeAt(0 / 0));
console.log(s.charCodeAt(1 / 0));
console.log("".charCodeAt(0));

const emoji = "🌍";
console.log(emoji.length);
console.log(emoji.charCodeAt(0));
console.log(emoji.charCodeAt(1));
console.log(emoji.charCodeAt(2));

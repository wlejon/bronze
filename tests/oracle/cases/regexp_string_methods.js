// The String.prototype methods that take a pattern (ECMA-262 22.1.3), each of
// which is defined by delegating to a RegExp symbol method in 22.2.6.
//
// Expectations derived from: 22.1.3.19 replace / 22.2.6.11 [@@replace] and
// 22.1.3.19.1 GetSubstitution (which fixes what `$$`, `$&`, `$\``, `$'`,
// `$n` and `$<name>` mean, and that a `$n` naming a group the pattern does
// not have stays literal), 22.1.3.20 replaceAll (which requires `g` when the
// pattern is a RegExp), 22.1.3.23 split / 22.2.6.14 [@@split] (capture groups
// are spliced into the result and the limit truncates it), 22.1.3.21 search /
// 22.2.6.12 (which restores lastIndex), 22.1.3.15 match / 22.2.6.8 (a global
// pattern yields the match TEXTS and null when there are none, a non-global
// one yields the match array), and 22.1.3.16 matchAll / 22.2.6.9.
console.log("a1b22c".replace(/\d+/, "#"));
console.log("a1b22c".replace(/\d+/g, "#"));
console.log("a1b22c".replaceAll(/\d+/g, "#"));
console.log("a-b-c".replaceAll("-", "+"));
console.log("x".replace("x", "$&$&"));
console.log("2026-08-11".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"));
console.log("abc".replace(/b/, "[$`|$'|$&]"));
console.log("abc".replace(/b/, "$$"));
console.log("abc".replace(/b/, "$9"));
console.log("2026-08".replace(/(?<y>\d{4})-(?<m>\d{2})/, "$<m>/$<y>"));

// The replacer is called with (matched, ...captures, offset, string).
console.log("aaa".replace(/a/g, function (s, i) { return i; }));
console.log("a1".replace(/([a-z])(\d)/, function (m, p1, p2, off, str) {
  return p2 + p1 + off + str.length;
}));

console.log("a,b;c".split(/[,;]/).join("|"));
console.log("a1b2c".split(/(\d)/).join("|"));
console.log("aaa".split(/a/).length);
console.log("abc".split(/x/).join("|"));
console.log("a,b,c".split(/,/, 2).join("|"));

console.log("hello".search(/l/), "hello".search(/z/));
// search must not disturb the cursor it borrowed.
const cursor = /l/g;
cursor.lastIndex = 4;
console.log("hello".search(cursor), cursor.lastIndex);

console.log("hello world".match(/o/g).join("|"));
console.log("hello".match(/z/g));
const one = "hello".match(/l(l)/);
console.log(one[0], one[1], one.index);

let seen = "";
for (const mm of "a1b2".matchAll(/([a-z])(\d)/g)) {
  seen = seen + mm[0] + ":" + mm[1] + mm[2] + "@" + mm.index + ";";
}
console.log(seen);

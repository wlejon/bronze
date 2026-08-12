// The corners of String.prototype: slice's negative indices against substring's
// swap, charAt's "" against at's undefined for an out-of-range index, the empty
// needle, split with no separator against split with an empty one, and padding
// with a multi-character filler. Every expectation is ECMA-262, derived by
// hand.
const s = "Hello, World";
console.log(s.slice(-5));
console.log(s.substring(5, 0));
console.log(s.slice(5, 0) === "");
console.log(s.indexOf("o", 5));
console.log(s.lastIndexOf("o"));
console.log("".charAt(0) === "");
console.log(s.at(-20));
console.log("abc".indexOf(""));
console.log("abc".includes(""));
console.log("a-b-c".split("-").length);
console.log("abc".split("").join("|"));
console.log("abc".split(undefined).length);
console.log("aaa".replace("a", "b"));
console.log("aaa".replaceAll("a", "b"));
console.log("x".padStart(4, "ab"));
console.log("x".padEnd(4, "ab"));
console.log("  x  ".trimStart() + "|");
console.log("  x  ".trimEnd() + "|");
console.log("abc".startsWith("b", 1));
console.log("abc".endsWith("b", 2));
console.log("abc".concat("d", 1));
console.log("ab".repeat(0) === "");

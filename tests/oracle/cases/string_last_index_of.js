// String.prototype.lastIndexOf's position argument (ECMA-262 22.1.3.11): the
// search starts at min(ToIntegerOrInfinity(position), len - needle.length) and
// walks down; a NaN position (absent or undefined included) searches from the
// end. The textarea line-start idiom `value.lastIndexOf('\n', pos - 1) + 1`
// depends on it.
const t = "ab\ncd\nef";
console.log(t.lastIndexOf("\n", 3));
console.log(t.lastIndexOf("\n", 5));
console.log(t.lastIndexOf("\n", 1));
console.log(t.lastIndexOf("\n", -1) + 1);
console.log("abc".lastIndexOf("c", 1));
console.log("abc".lastIndexOf("c", 2));
console.log("a\nb\nc".lastIndexOf("\n", 2));
console.log("abcabc".lastIndexOf("abc", 2));
console.log("abcabc".lastIndexOf("abc", 3));
console.log("abcabc".lastIndexOf("abc", 99));
console.log("abcabc".lastIndexOf("abc", Infinity));
console.log("abcabc".lastIndexOf("abc", -Infinity));
console.log("abcabc".lastIndexOf("abc", NaN));
console.log("abcabc".lastIndexOf("abc", undefined));
console.log("abcabc".lastIndexOf("abc", "1"));
console.log("abcabc".lastIndexOf("abc", 3.9));
console.log("abc".lastIndexOf("", 1));
console.log("abc".lastIndexOf("", 10));
console.log("abc".lastIndexOf(""));
console.log("abc".lastIndexOf("abcd"));
console.log("".lastIndexOf(""));
console.log("aaa".lastIndexOf("a", 0));
console.log("aaa".lastIndexOf("aa", 2));
const pos = { valueOf() { return 4; } };
console.log("abcabc".lastIndexOf("b", pos));

// String.prototype beyond `length` and `charCodeAt`, which are the only two
// bronze implements. The rest are named hard errors today. Strings are
// immutable (docs/0004), so every one of these returns a fresh string and
// none of them can be done in place.
const s = "Hello, World";
console.log(s.length);
console.log(s.toUpperCase());
console.log(s.toLowerCase());
console.log(s.indexOf("World"));
console.log(s.indexOf("zzz"));
console.log(s.charAt(1));
console.log(s.slice(7));
console.log(s.slice(0, 5));
console.log(s.substring(7, 12));
console.log(s.includes("lo,"));
console.log(s.startsWith("Hello"));
console.log(s.endsWith("World"));
console.log(s.replace("World", "there"));
console.log("  pad  ".trim());
console.log("ab".repeat(3));
console.log(s.split(", ").join("|"));
console.log(s.at(-1));

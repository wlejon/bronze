// A string literal denotes characters, not the source text between the
// quotes. Every expectation is ECMA-262, derived by hand (docs/0003):
// escapes resolve, an unrecognized escape denotes its own character, and a
// literal's length is counted in UTF-16 code units, so an astral character
// is 2 and a Latin-1 one is 1.
console.log("q\n".length);
console.log("a\tb".length);
console.log("\\".length);
console.log("\x41\u0042\u{43}");
console.log("it's \"quoted\"");
console.log("\q");
console.log("\u00e9".length);
console.log("\u{1F600}".length);
console.log("\uD83D\uDE00".length);
console.log("aéb".charCodeAt(1));
console.log("line1\nline2");

// Lookbehind, past the four lines regexp_lookbehind.js pins. Everything here
// is 22.2.2.6's backward `direction` reaching a part of the matcher the plain
// case does not: the counted-repeat path, a second lookaround nested in the
// first, an Alternative's ordering, and an Assertion that consumes nothing.

// A quantifier inside a lookbehind. `a{2,3}` backward is the same COUNT the
// forward path takes, scanned the other way — which is what keeps `(?<=a*)`
// from needing a stack frame per repetition.
console.log(/(?<=a{2,3})b/.test("aab"), /(?<=a{2,3})b/.test("ab"));
console.log(/(?<=a*)b/.test("b"), /(?<=x+)y/.test("xxxy"));

// A lookbehind inside a lookahead. The inner one runs backward from wherever
// the outer one has reached, and what follows the outer one resumes FORWARD:
// direction belongs to the assertion, not to the match.
console.log(/a(?=b(?<=ab))/.test("ab"), /a(?=b(?<=xb))/.test("ab"));

// Both groups are still greedy right to left, so the SECOND one takes all four
// digits first and gives back only what the first one needs.
const digits = /(?<=(\d+)(\d+))$/.exec("1053");
console.log(digits[1], digits[2]);

// And a lazy one still takes the fewest turns, so group 1 is one `a` and the
// match begins at the last one.
const lazy = /(?<=(a+?))b/.exec("aaab");
console.log(lazy[1], lazy.index);

// An Alternative inside a lookbehind is ordered like any other Disjunction;
// only the terms WITHIN an alternative reverse.
console.log(/(?<=ab|b)c/.test("abc"), /(?<=cd|b)c/.test("abc"));

// `\b` and `^` consume nothing whichever way they ran, so they ask about the
// position the backward walk has reached and not about a unit it took.
console.log(/(?<=\bfoo)bar/.test("foobar"), /(?<=\bfoo)bar/.test("xfoobar"));
console.log(/(?<=^ab)c/.test("abc"), /(?<=^b)c/.test("abc"));

// A negative lookbehind at index 0 has nothing behind it to match, which is
// exactly what makes it hold.
console.log("1a2".replace(/(?<![a-z])\d/g, "#"));
console.log(/(?<!\d)\d{3}/.exec("ab123")[0]);

// An assertion consumes nothing, so a lookbehind on its own matches the empty
// string at every position it holds at.
console.log("aaa".replace(/(?<=a)/g, "-"));

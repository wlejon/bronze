// `\w` under `i`, which is the one class escape whose SET the flag can change
// (22.2.2.7.1 WordCharacters) -- and, for an engine with no `u` flag, does not.
//
// WordCharacters is the basic sixty-three (`a-z A-Z 0-9 _`), plus every
// character that is not one of them whose Canonicalize IS one of them. Step 3
// asserts that second set is EMPTY unless [[Unicode]] and [[IgnoreCase]] are
// both true, and bronze has no `u` flag -- so `\w` is the same sixty-three with
// the flag as without it, and `\W` is its complement either way.
//
// U+017F LATIN SMALL LETTER LONG S and U+212A \u212a SIGN are the two that look
// like members and are not. Their uppercases are `S` and `K`, but 22.2.2.9
// Canonicalize returns a non-ASCII character UNCHANGED when its uppercase is
// ASCII, so neither canonicalizes to a word character and neither joins the
// set. The first line below is the whole argument in one place: the set a
// pattern builds and the comparison the matcher runs have to answer the same
// way about U+017F, and the long s against an "s" is that comparison.
//
// The last line is why this is a case and not only a unit test. `\b` consults
// the same WordCharacters, so a `\w` that had grown two members would move word
// boundaries in patterns that never mention `\w` at all.
//
// Every character above U+007F is spelled by escape, because the difference
// between an ASCII `K` and U+212A \u212a SIGN is the entire content of the
// second line and no reader should have to take it on trust.

// The set and the comparison, on the same character.
console.log(/\w/i.test("\u017f"), /\u017f/i.test("s"));
console.log(/\w/i.test("\u212a"), /\w/.test("\u212a"));

// `\W` is the complement of the same set, so it follows rather than being
// decided separately -- and a negated class agrees with it.
console.log(/\W/i.test("\u017f"), /\W/i.test("\u212a"));
console.log(/[^\w]/i.test("\u017f"), /[\w]/i.test("\u017f"));

// A whole-string match, which is how the difference shows up in real patterns:
// long-s followed by `k` is not a word at all, in either mode.
console.log(/^\w+$/i.test("\u017fk"), /^\w+$/.test("\u017fk"));

// The basic sixty-three are still there, and `i` still folds within them.
console.log(/\w/i.test("K"), /\w/i.test("k"), /\w/i.test("_"), /\w/i.test("9"));

// A global replace over a string holding one: `\w` steps over it and `\W`
// takes it. The survivor is read back by code unit so this line stays ASCII.
console.log("a\u017fb".replace(/\w/gi, "-").charCodeAt(1), "a\u017fb".replace(/\W/gi, "-"));

// `\b` is WordCharacters too, so a `\w` with two extra members would silently
// move every boundary beside one.
console.log(/a\b/i.test("a\u017f"), /a\B/i.test("a\u017f"));

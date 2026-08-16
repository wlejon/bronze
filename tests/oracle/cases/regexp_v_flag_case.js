// What `v` changes about case-insensitive matching, which is the half of the
// flag that is not about set operators at all.
//
// Under `u` with `i`, 22.2.2.7.1 canonicalizes the input character and every
// candidate member and THEN inverts the answer for a `[^...]`. Under `v` the
// complement is taken over a set instead, at compile time, and 22.2.2.9's
// AllCharacters says which alphabet: with both `v` and `i` it is the code
// points that are their own simple case folding — not the whole code space.
//
// That one choice is the entire difference, and it is worth stating why it
// exists. `\P{X}` under `ui` is "every code point not in X", which CONTAINS the
// uppercase letters when X is the lowercase ones; inverting membership in that
// set afterwards therefore rejects `A` twice over, and `[^\P{Ll}]` — read by
// anyone as "the lowercase letters" — matches nothing whatsoever. Under `v` the
// complement runs over an alphabet the fold has already collapsed, so
// `[^\p{X}]`, `[\P{X}]` and `\P{X}` all mean one thing, and so do their three
// positive twins. The first four lines pin exactly that.
//
// The last lines pin the other direction: which behaviours must NOT change. The
// fold `v` uses is the same simple case folding `u` uses — the same table, the
// same U+017F and U+212A — and a class with no set operation in it answers the
// same under either flag.

// The example the `v` proposal is argued from. Both patterns are written to
// mean "the lowercase letters"; under `u` only one of them does.
console.log("aAbBcC4#".replace(/\p{Lowercase_Letter}/giu, "X"),
            "aAbBcC4#".replace(/[^\P{Lowercase_Letter}]/giu, "X"));
console.log("aAbBcC4#".replace(/\p{Lowercase_Letter}/giv, "X"),
            "aAbBcC4#".replace(/[^\P{Lowercase_Letter}]/giv, "X"));

// The same asymmetry in one escape and one character. `\P{Ll}` under `v` is the
// fold's fixed points minus the lowercase letters, and `A` is not one of those
// fixed points; under `u` it is every code point but the lowercase letters, and
// `A` is one of those.
console.log(/\P{Lowercase_Letter}/vi.test("A"), /\P{Lowercase_Letter}/ui.test("A"));

// The identity `v` restores: three spellings of the complement agree, and three
// spellings of the property agree.
console.log(/[^\p{Ll}]/vi.test("A"), /[\P{Ll}]/vi.test("A"), /\P{Ll}/vi.test("A"));
console.log(/[^\P{Ll}]/vi.test("A"), /[\p{Ll}]/vi.test("A"), /\p{Ll}/vi.test("A"));

// A pair that does NOT differ, pinned so the difference above is not read as a
// general one. U+212A KELVIN SIGN folds onto `k` under either flag, so both
// modes reject it for `[^k]` — one by removing `k` from an alphabet that never
// held the Kelvin sign, the other by canonicalizing onto the `k` it inverts.
// Without `i` there is no fold and it is simply another character.
console.log(/[^k]/vi.test("K"), /[^k]/ui.test("K"), /[^k]/v.test("K"));

// Where the fold enters a set OPERATION. Without it an intersection of two
// spellings of one letter would be empty, and a difference would remove only
// the spelling it was written with.
console.log(/^[[a]&&[A]]$/vi.test("A"), /^[[a-e]--[C]]$/vi.test("c"),
            /^[[a-e]--[C]]$/v.test("c"));
console.log(/[^K]/vi.test("k"), /[K]/vi.test("k"), /[^K]/v.test("k"));

// The table itself is unchanged: `v` with `i` reads CaseFolding.txt exactly as
// `u` with `i` does, and `i` alone still reads the uppercase mapping instead —
// which is why U+017F matches `s` under the first two and not the third.
console.log(/ſ/vi.test("s"), /ſ/ui.test("s"), /ſ/i.test("s"));

// 22.2.6.4 spells the flags in one fixed order, `d g i m s u v y`, whatever
// order they were written in.
console.log(/a/vi.flags, /a/giv.flags);

// And the two halves compose: a complement, folded, intersected against a
// difference. `b` survives because it was subtracted; `B` survives because the
// fold maps it onto the `b` that was.
console.log("aAbBcC4#dD".replace(/[[^\P{Ll}]--[b]]/giv, "X"));

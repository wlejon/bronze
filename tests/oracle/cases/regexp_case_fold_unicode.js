// Case folding above Latin-1, from 22.2.2.9 Canonicalize and 22.2.2.7.1 (a
// class matches when the SET holds a member canonicalizing to the same code
// point the input does, which is what makes a RANGE fold).
//
// Canonicalize is defined by toUppercase from the Unicode Default Case
// Conversion table, and bronze carries no Unicode data file. It implements
// exactly the part of that table it can state in code and check by hand: each
// block written as the RULE that generated it, plus the members that break the
// rule. The two blocks here have the plainest rules of any — Greek, a +32
// offset from U+0391 to U+03A9, and Cyrillic, a +32 offset from U+0410 to
// U+042F.
//
// Everything bronze has no rule for stays refused at COMPILE time and by code
// point, because a wrong fold is invisible: `/Ω/i` silently not matching
// a lowercase omega is a wrong answer a test would only catch if someone
// thought to write it. The refusal costs nothing to programs that do not use
// `i` over cased scripts — a pattern of CJK or emoji still takes `i`.
//
// The last two lines are the table's reverse direction. A range folds only
// because U+03B3 canonicalizes to the code point U+0393 does, which is not
// something canonicalizing the input alone can ever find.
//
// tests/oracle/cases/regexp_case_fold_blocks.js carries the members that break
// the rules — the final sigma, the dotless i whose uppercase is ASCII, and
// the two units `\w` itself grows under `i`.
console.log(/\u03a9/i.test("\u03c9"));
console.log(/\u0410/i.test("\u0430"));
console.log(/[\u03b1-\u03c9]/i.test("\u0393"));
console.log("\u0391\u0392".replace(/[\u03b1-\u03c9]/gi, "-"));

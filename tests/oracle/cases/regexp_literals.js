// The RegExp object's own surface: the members a program reads off one, and
// the array `exec` builds (docs/0024).
//
// Expectations derived from ECMA-262 22.2.6.5 (`flags`, and the order it
// spells them in), 22.2.6.10 (`source` is the pattern text as written),
// 22.2.6.9 (`lastIndex`), 22.2.7.2 (RegExpBuiltinExec: the match array, its
// `index`, `input` and `groups`, and when `lastIndex` moves), 22.2.6.13
// (`toString`), and docs/0013 for console.log's container format.
const re = /ab+c/;
console.log(re.source);
console.log("[" + re.flags + "]");
console.log(re.global, re.ignoreCase, re.multiline, re.dotAll, re.sticky);
console.log(re);
console.log("" + re);
console.log(re.test("xxabbbcyy"));
console.log(re.test("xxacyy"));

// Every flag reported, in 22.2.6.5's order whatever order they were written.
const all = /x/yims;
console.log(all.flags);
console.log(all.global, all.ignoreCase, all.multiline, all.dotAll, all.sticky);

// exec, and the array it answers with. Group 2 did not participate, so it is
// `undefined` and not the empty string — a distinction 22.2.7.2 step 28
// depends on.
const g = /a(b)(c)?/g;
console.log(g.flags, g.global, g.lastIndex);
const m = g.exec("zzabzz");
console.log(m[0], m[1], m[2]);
console.log(m.index, m.input);
console.log(m.length);
console.log(m);
console.log(g.lastIndex);

// Exhausted: `null`, and the cursor goes back to the start so the next call
// begins again (22.2.7.2 step 12.a.ii).
console.log(g.exec("zzabzz"));
console.log(g.lastIndex);

// A non-global pattern ignores `lastIndex` entirely and never writes it.
const plain = /a/;
plain.lastIndex = 5;
console.log(plain.test("a"), plain.lastIndex);

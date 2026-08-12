// Lookbehind assertions, from 22.2.1 (Assertion :: `(?<=` Disjunction `)` and
// `(?<!` Disjunction `)`) and 22.2.2.6 (the backward direction, and that an
// assertion consumes nothing whichever way it ran).
//
// The grammar is the easy half: `(?<=` and `(?<!` parse like the lookaheads
// beside them. What this case is really for is 22.2.2.6's `direction`, which
// matches a lookbehind's Disjunction backward — the terms of an Alternative
// taken in REVERSE order, each Atom still greedy, and every quantifier and
// capture inside running against a decreasing index.
//
// The first line is why a lookbehind cannot be quietly dropped: without one,
// `/(?<=\$)\d+/` matches every number in the string rather than the priced
// one. The last is where the backward direction is visible in the ANSWER and
// not just in whether there was one.
//
// tests/oracle/cases/regexp_lookbehind_edges.js carries the rest — a
// quantifier inside a lookbehind, a lookbehind inside a lookahead, an
// alternation, and the negative form at the start of the input.
console.log("price: $42".replace(/(?<=\$)\d+/, "99"));
console.log(/(?<=a)b/.test("ab"), /(?<=a)b/.test("cb"));
console.log(/(?<!a)b/.test("ab"), /(?<!a)b/.test("cb"));

// Matched right to left, so the LAST term of the lookbehind is the one that
// starts at the assertion's position and the first term ends up leftmost.
const back = /(?<=([ab]+)([bc]+))$/.exec("abc");
console.log(back[1], back[2]);

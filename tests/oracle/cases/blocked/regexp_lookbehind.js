// BLOCKED: `unsupported: lookbehind assertions `(?<=` and `(?<!` are not
// implemented`, named at the literal by src/regex/parser.cpp.
//
// The pattern grammar of ECMA-262 22.2.1 is built except for this one
// production. The reason is not the grammar — `(?<=` and `(?<!` parse like the
// lookaheads beside them — but 22.2.2.6, which matches a lookbehind's
// Disjunction with `direction` = backward: the terms of an Alternative are
// taken in REVERSE order, each Atom still greedy, and every quantifier and
// capture inside runs against a decreasing index. bronze's matcher threads a
// forward continuation through every node (`Cont` holds the NEXT term), so
// direction is not a parameter it has; giving it one means a second traversal
// order through matchSequence, matchRepeat and matchSimpleRepeat, and a
// backward form of the counted-repeat fast path or a stack-depth regression on
// `(?<=a*)`.
//
// It is named rather than silently dropped because a lookbehind that did
// nothing would make `/(?<=\$)\d+/` match every number in the string — the
// exact shape of silent wrong answer the hard-error rule exists to prevent.
//
// What this case pins when it lands, from 22.2.1 (Assertion :: `(?<=`
// Disjunction `)` and `(?<!` Disjunction `)`) and 22.2.2.6 (the backward
// direction, and that an assertion consumes nothing whichever way it ran):
console.log("price: $42".replace(/(?<=\$)\d+/, "99"));
console.log(/(?<=a)b/.test("ab"), /(?<=a)b/.test("cb"));
console.log(/(?<!a)b/.test("ab"), /(?<!a)b/.test("cb"));

// Matched right to left, so the LAST term of the lookbehind is the one that
// starts at the assertion's position and the first term ends up leftmost.
const back = /(?<=([ab]+)([bc]+))$/.exec("abc");
console.log(back[1], back[2]);

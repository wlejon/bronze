// BLOCKED, and it is a SILENT WRONG ANSWER rather than a refusal: `"a" < "b"`
// is `false`, and `"b" <= "a"` is `true`.
//
// `<` and `>` lower to `CmpLt`/`CmpGt`, and lower_expr_binary.cpp sends those
// two down the F64 path whatever their operands are. Both sides go through
// ToNumber, a non-numeric string becomes NaN, and every comparison against NaN
// is false. `<=` and `>=` are then compiled as the NEGATIONS of `>` and `<`, so
// the same NaN makes them uniformly TRUE. That is the worse half: `<` is at
// least consistently wrong, while `"b" <= "a"` is a confident yes.
//
// ECMA-262 13.10.1 IsLessThan is where the missing step lives. After ToPrimitive
// on both operands, step 3 asks whether BOTH are Strings, and if they are it
// compares them by code unit (step 3.c, via 7.2.13 IsStringLessThan) and never
// converts anything. ToNumeric is step 4, the branch taken only when that test
// fails. bronze goes straight to step 4.
//
// It ranks with `print_collections`'s crash rather than with an unimplemented
// member: nothing says anything. A program that sorts names, bisects a sorted
// array of strings, or range-checks a version tag gets an answer of the right
// TYPE and the wrong value, and the oracle suite cannot catch what no case
// evaluates. Comparing strings is not an exotic corner of the language.
//
// It survived because no case compared two strings with an inequality. The
// suite has thorough coverage of `===`, `==` and every arithmetic operator, and
// `cases/binary_math` walks the relational operators over numbers only.
//
// The expectation is derived from 13.10.1 and 7.2.13, which compare UTF-16 code
// units, not characters and not any collation:
//
// 1. Ordinary ordering, and the `<=`/`>=` forms whose negation is the bug.
// 2. `"2" < "10"` is FALSE where `2 < 10` is true — the case that proves a
//    numeric string is compared as text, since step 3 fires before step 4 ever
//    looks at the digits.
// 3. `"Z" < "a"` is true because 0x5A precedes 0x61. Code units, so uppercase
//    sorts before lowercase; a locale-aware answer would be a different one and
//    `localeCompare` is deliberately unimplemented (rt_members.cpp).
// 4. A prefix is less than what extends it (7.2.13 step 3).
// 5. A real use — filtering by a bound — because the operator being wrong in
//    isolation is not what damages a program.
// 6. When only ONE side is a string, step 3 fails and step 4 is correct:
//    `"10" < 9` is a numeric comparison and `"a" < 1` is a NaN one, so both
//    are false and both already work. They are here so that fixing this cannot
//    regress them.
//
// When it passes, promote it and rewrite this header to say what it pins.

console.log("a" < "b", "b" < "a", "abc" < "abd");
console.log("b" <= "a", "a" <= "a", "b" >= "a");
console.log("2" < "10", 2 < 10);
console.log("Z" < "a", "apple" < "apples");

const names = ["banana", "apple", "cherry"];
console.log(names.filter(function (x) { return x < "c"; }).join(","));

console.log("10" < 9, "a" < 1);

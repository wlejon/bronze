// Two strings compared with an inequality: ECMA-262 13.10.1 IsLessThan step 3,
// which compares them by CODE UNIT and converts nothing.
//
// Step 3 asks whether both operands are Strings after ToPrimitive, and answers
// through 7.2.13 IsStringLessThan when they are. ToNumeric is step 4, the
// else-branch — so a string operand is never read as a number while the other
// side is a string too, and `"2" < "10"` is true where `2 < 10` is false.
//
// What each line holds:
//
// 1. Ordinary ordering, and the `<=` / `>=` forms, whose equal case (`"a" <=
//    "a"`) is the one an ordering alone does not settle.
// 2. `"2" < "10"` beside `2 < 10`, the pair that proves step 3 fires before
//    step 4 ever looks at the digits.
// 3. `"Z" < "a"` is true because 0x5A precedes 0x61: code units, so uppercase
//    sorts before lowercase. A locale-aware answer would differ, and this is
//    deliberately not one — deterministic output is a house rule and
//    `localeCompare` stays unimplemented (rt_members.cpp) because of it.
//    Beside it, 7.2.13 step 3: a prefix is less than what extends it.
// 4. A filter by a bound, because an operator being right in isolation is not
//    what a program depends on.
// 5. When only ONE side is a string, step 3 fails and step 4 is the answer:
//    `"10" < 9` is a numeric comparison and `"a" < 1` a NaN one, so both are
//    false. They are here so that the string branch cannot regress them.
//
// This is a sibling of `cases/relational_nan`, and they share an edit in
// lower_expr_binary.cpp without being the same fact: that case is about a
// rewrite that is invalid on its own terms, and this one about a branch of the
// algorithm that was never taken.

console.log("a" < "b", "b" < "a", "abc" < "abd");
console.log("b" <= "a", "a" <= "a", "b" >= "a");
console.log("2" < "10", 2 < 10);
console.log("Z" < "a", "apple" < "apples");

const names = ["banana", "apple", "cherry"];
console.log(names.filter(function (x) { return x < "c"; }).join(","));

console.log("10" < 9, "a" < 1);

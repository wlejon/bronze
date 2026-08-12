// The four function properties of the global object that ECMA-262 19.2
// defines over numbers (docs/0027 decision 4). `Ray.js` and `Color.js` in the
// three.js closure both call them.
//
// From 19.2.2 (isFinite), 19.2.3 (isNaN), 19.2.4 (parseFloat), 19.2.5
// (parseInt) and 21.1.2.12/13:
//
// 1. The two PREDICATES coerce: step 1 of each is ToNumber. So `isNaN("x")`
//    is true where `Number.isNaN("x")` is false, and `isNaN(null)` is false
//    because ToNumber(null) is +0. That contrast is the whole reason the
//    `Number` statics exist beside these, and both halves are pinned here.
// 2. `parseInt` (19.2.5): trims LEADING white space only; takes an optional
//    sign; ToInt32s the radix, so a radix of NaN or Infinity is 0 and selects
//    the default; strips a `0x`/`0X` prefix and switches to radix 16 only when
//    the radix argument was absent-or-0 or was already 16; rejects a radix
//    outside 2..36; reads the longest prefix of radix digits and answers NaN
//    when that prefix is empty. `"08"` is 8 — there has been no octal since
//    ES5. `"-0x10"` is -16: the sign is removed before the prefix is looked
//    for. `"- 42"` is NaN: white space after the sign is not white space
//    before the number.
// 3. `parseFloat` (19.2.4): the longest prefix that is a StrDecimalLiteral,
//    which is why `"3.5px"` is 3.5 where ToNumber of it is NaN. `"Infinityx"`
//    is Infinity, `".5e1"` is 5, and `"1e"` is 1 because an ExponentPart needs
//    digits, so the longest literal prefix stops before the `e`. Hexadecimal
//    is not a StrDecimalLiteral, so `"0x10"` is 0 — just the leading `"0"`.
// 4. Both take a STRING: step 1 of each is ToString of the argument. That is
//    what makes `parseInt(1e21)` 1 — ToString(1e21) is "1e+21" and the digit
//    prefix is "1" — and `parseInt(0.0000005)` 5, from "5e-7".
// 5. 21.1.2.12 and 21.1.2.13: `Number.parseFloat` and `Number.parseInt` are
//    "the same function object" as the global ones, so `===` holds. The
//    predicates are NOT the same function objects, because they are not the
//    same functions.

console.log(isNaN("x"), isNaN("42"), isNaN(""), isNaN("  "));
console.log(isNaN(NaN), isNaN(undefined), isNaN(null), isNaN(true));
console.log(isNaN("Infinity"), isNaN("0x10"), isNaN(0 / 0));

console.log(isFinite("1"), isFinite(Infinity), isFinite(-Infinity), isFinite(NaN));
console.log(isFinite(null), isFinite(undefined), isFinite("abc"), isFinite(1e308));

console.log(parseInt("08"), parseInt("0x1f"), parseInt("12abc"), parseInt("  42  "));
console.log(parseInt("", 10), parseInt("-0x10"), parseInt("+7"), parseInt("- 42"));
console.log(parseInt("0x10", 16), parseInt("0x10", 10), parseInt("10", 2), parseInt("z", 36));
console.log(parseInt("10", 0), parseInt("10", 1), parseInt("10", 37), parseInt("10", 2.9));
console.log(parseInt("10", NaN), parseInt("10", Infinity), parseInt("Infinity"));
console.log(parseInt(15.99), parseInt(1e21), parseInt(0.0000005));
console.log(parseInt(true), parseInt(null), parseInt(undefined));

console.log(parseFloat(".5e1"), parseFloat("Infinityx"), parseFloat("1e"), parseFloat("1e+2"));
console.log(parseFloat("  3.5rest"), parseFloat("+.5"), parseFloat("-Infinity"));
console.log(parseFloat("."), parseFloat("0x10"), parseFloat("abc"), parseFloat(""));
console.log(parseFloat(1e21), parseFloat(-0.5), parseFloat(true), parseFloat(null));

console.log(parseInt === Number.parseInt, parseFloat === Number.parseFloat);
console.log(isNaN("NaN"), Number.isNaN("NaN"));
console.log(isFinite("1"), Number.isFinite("1"));

// The globals are shadowable like every other provided name (docs/0011
// decision 1): a local declaration wins with no special case.
{
  const parseInt = function () {
    return "shadowed";
  };
  console.log(parseInt("0x1f"));
}
console.log(parseInt("0x1f"));

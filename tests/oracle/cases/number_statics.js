// The `Number` namespace: the predicates and the numeric constants, which are
// the everyday alternative to the global coercions bronze deliberately does not
// provide.
//
// From ECMA-262 21.1.2 (Number statics), 19.2.4 (parseFloat) and 19.2.5
// (parseInt):
//
// 1. The three predicates do NOT coerce. `Number.isNaN("NaN")` is false and
// `Number.isInteger("5")` is false, which is the whole reason they exist beside
// the global functions of the same names. 2. The constants are pinned as bytes,
// so this case is also a shortest round-trip test of the float formatter:
// `Number.EPSILON` and `Number.MIN_VALUE` have no shorter decimal form that
// reads back as the same double, and both go through std::to_chars. 3.
// `parseInt` reads a PREFIX, skips leading whitespace, honours a `0x` prefix
// when no radix is given, takes an explicit radix, and answers NaN when no
// digits were consumed. `parseFloat` reads a prefix the same way and accepts
// "Infinity", which `Number("Infinity")` also does but `parseInt` does not.
//
// `Number.prototype.toFixed` is NOT here: it belongs to the Number wrapper
// rather than the namespace, and it is pinned in cases/blocked/.

console.log(Number.isInteger(5), Number.isInteger(5.5), Number.isInteger("5"));
console.log(Number.isNaN(NaN), Number.isNaN("NaN"), Number.isNaN(0 / 0));
console.log(Number.isFinite(Infinity), Number.isFinite(1), Number.isFinite("1"));
console.log(Number.isSafeInteger(9007199254740991), Number.isSafeInteger(9007199254740992));
console.log(Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER);
console.log(Number.EPSILON, Number.MAX_VALUE, Number.MIN_VALUE);
console.log(Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NaN);
console.log(Number.parseInt("42px"), Number.parseInt("  -17  "), Number.parseInt("0x1f"));
console.log(Number.parseInt("ff", 16), Number.parseInt("101", 2), Number.parseInt("z"));
console.log(Number.parseFloat("3.25rest"), Number.parseFloat(".5"), Number.parseFloat("1e3"));
console.log(Number.parseFloat("Infinity"), Number.parseFloat("abc"));
console.log(Number.parseInt("Infinity"), Number.parseFloat("-0.125e2"));

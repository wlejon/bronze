// `==` and `!=`: the abstract equality comparison of ECMA-262 7.2.14. Before
// this the three cases below were compile errors that told the user to write
// `===` instead, which is not what the language says the operator means.
//
// The algorithm in the order the spec states it: same type is strict
// equality; null and undefined are equal to each other and to nothing else;
// a number against a string coerces the STRING with ToNumber; a boolean
// operand is ToNumber'd and the comparison restarts. Object-against-object
// is identity.
console.log(null == undefined);
console.log(undefined == null);
console.log(null == null);
console.log(undefined == undefined);

// ...and to nothing else. `null == 0` is false even though ToNumber(null)
// is 0, because the algorithm never reaches ToNumber for a null operand.
console.log(null == 0);
console.log(null == false);
console.log(null == "");
console.log(null == NaN);
console.log(undefined == 0);
console.log(undefined == false);
console.log(undefined == "");

// Number against string: the string is ToNumber'd.
console.log(1 == "1");
console.log("1" == 1);
console.log(0 == "");
console.log(0 == " ");
console.log(0 == "0");
console.log(2 == "2.0");
console.log(1 == "1x");
console.log("" == "0");
console.log("abc" == "abc");
console.log("abc" == "abd");

// A boolean is ToNumber'd first, and then the rule for what is left applies.
// So `true == "1"` becomes `1 == "1"` becomes `1 == 1`.
console.log(true == 1);
console.log(true == "1");
console.log(true == "true");
console.log(false == 0);
console.log(false == "");
console.log(false == "0");
console.log(true == 2);

// Same-type comparisons are strict equality, NaN and -0 included.
console.log(NaN == NaN);
console.log(0 == -0);
console.log(-0 == 0);
console.log(1 == 1);
console.log(true == true);
console.log(true == false);

// Objects compare by identity, and an object is never equal to null or
// undefined (the algorithm answers false before any coercion).
const o = { a: 1 };
const same = o;
console.log(o == same);
console.log(o == {});
console.log(o == null);
console.log(o == undefined);
const arr = [1];
console.log(arr == arr);
console.log(arr == [1]);

// `!=` is the negation of `==`, computed from the same algorithm.
console.log(1 != 2);
console.log(1 != 1);
console.log(null != undefined);
console.log(null != 0);
console.log(1 != "1");
console.log(NaN != NaN);

// `===` across distinct types is false without any coercion, which is the
// difference these two cases exist to show.
console.log(1 === "1");
console.log(1 === true);
console.log(null === undefined);
console.log(0 === -0);
// NaN is the one value not equal to itself, under BOTH equality operators.
// `NaN !== NaN` printed false before this case asked: the negated form was
// folded into an ordered floating-point compare, which is the right test for
// numeric truthiness (`if (NaN)` is false) and the wrong one for `!==`.
console.log(NaN === NaN);
console.log(NaN !== NaN);

// Equality is looser than the relational operators and tighter than `&&`.
console.log(1 < 2 == true);
console.log(1 == 1 && 2 == 2);

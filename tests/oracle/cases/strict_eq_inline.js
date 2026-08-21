// `===` at every row of ECMA-262 7.2.16 IsStrictlyEqual, because generated
// code now answers three of those rows itself (llvm_arith.cpp emitStrictEq)
// and only reaches `bronze_strict_eq` for a String or a BigInt on the left.
//
// The two IEEE-754 edges are the reason the inline path is three arms rather
// than one bit compare, so they come first and they are exhaustive:
//
//   - NaN is the value that is NOT equal to itself. Its BITS are equal to
//     itself, so an inline path that answered bit equality would say true.
//   - +0 and -0 are the values that ARE equal with different bits, so an
//     inline path that answered bit equality would say false.
//
// Everything is written so the answer follows from the spec text and not from
// running it: each line prints a boolean whose value 7.2.16 fixes.

// --- NaN, every way of making one -------------------------------------------
const nan = NaN;
const computed = 0 / 0;
const overflowed = Infinity - Infinity;
const parsed = Number('nope');
console.log(nan === nan, computed === computed, nan === computed);
console.log(overflowed === overflowed, parsed === parsed, nan === parsed);
// Through a variable the compiler cannot fold, and out of a typed array, where
// a payload NaN could survive if the value model let one.
const f64 = new Float64Array(2);
f64[0] = nan;
f64[1] = 0 / 0;
console.log(f64[0] === f64[0], f64[0] === f64[1], f64[0] === nan);
const f32 = new Float32Array(1);
f32[0] = nan;
console.log(f32[0] === f32[0], f32[0] === 0, isNaN(f32[0]));
// And the negation, which is the form three.js actually writes.
console.log(nan !== nan, computed !== nan, 1 !== 1);

// --- signed zero ------------------------------------------------------------
const posZero = 0;
const negZero = -0;
console.log(posZero === negZero, negZero === posZero, negZero === 0, 0 === -0);
console.log(1 / posZero === Infinity, 1 / negZero === -Infinity);
// Object.is is the operation that DOES separate them, and it must not have
// moved: if the inline arm leaked into it, this line changes.
console.log(Object.is(posZero, negZero), Object.is(nan, nan), Object.is(1, 1));
// -0 out of arithmetic, so the bits are produced rather than written.
const madeNegZero = -1 * 0;
console.log(madeNegZero === 0, Object.is(madeNegZero, -0));

// --- numbers that are ordinary ----------------------------------------------
console.log(1 === 1, 1 === 2, 1.5 === 1.5, 1 === 1.0);
console.log(Infinity === Infinity, -Infinity === Infinity, Infinity === -Infinity);
console.log(1e308 * 10 === Infinity, Number.MAX_SAFE_INTEGER === 9007199254740991);

// --- strings: equal by VALUE, not by identity -------------------------------
// The row the inline path deliberately refuses. Built so no two of these can
// be the same object: one is a literal, one is concatenated at run time, one
// comes out of an array join, one out of a slice.
const litA = 'material';
const builtA = 'mate' + 'rial'.slice(0);
const joinedA = ['m', 'a', 't', 'e', 'r', 'i', 'a', 'l'].join('');
const slicedA = 'xmaterialx'.slice(1, 9);
console.log(litA === builtA, litA === joinedA, litA === slicedA, builtA === joinedA);
console.log(litA === 'materia', litA === 'materials', litA === '', '' === '');
// A string against everything that is not a string.
console.log(litA === 1, litA === null, litA === undefined, litA === true);
console.log('1' === 1, '' === 0, '' === false, 'null' === null);
// And with the string on the RIGHT, which is the arm that has to answer
// `false` without asking the helper.
console.log(1 === litA, null === litA, undefined === litA, true === litA);

// --- BigInt: equal by mathematical value ------------------------------------
const bigA = 10n;
const bigB = BigInt('10');
const bigC = 5n + 5n;
console.log(bigA === bigB, bigA === bigC, bigA === 11n, 0n === -0n);
console.log(bigA === 10, 10 === bigA, bigA === '10', bigA === true);
console.log((2n ** 64n) === (2n ** 64n), (2n ** 64n) === (2n ** 64n + 1n));

// --- symbols: identity, and nothing else ------------------------------------
const s1 = Symbol('tag');
const s2 = Symbol('tag');
const s3 = s1;
console.log(s1 === s1, s1 === s3, s1 === s2);
console.log(Symbol.for('shared') === Symbol.for('shared'));
console.log(Symbol.iterator === Symbol.iterator, s1 === 'tag', s1 === undefined);

// --- boxed primitives are OBJECTS -------------------------------------------
// 7.2.16 step 1 makes an Object and a Number different types, so every one of
// these is false however equal the values look.
const boxedOne = new Number(1);
const boxedTwo = new Number(1);
const boxedStr = new String('material');
const boxedTrue = new Boolean(true);
console.log(boxedOne === 1, boxedOne === boxedTwo, boxedOne === boxedOne);
console.log(boxedStr === 'material', boxedStr === boxedStr, boxedTrue === true);
console.log(boxedOne == 1, boxedStr == 'material', boxedTrue == true);

// --- objects, null, undefined, booleans: identity ---------------------------
const o1 = { a: 1 };
const o2 = { a: 1 };
const o3 = o1;
const arr = [1];
console.log(o1 === o1, o1 === o3, o1 === o2, arr === arr);
console.log(null === null, undefined === undefined, null === undefined);
console.log(true === true, false === false, true === false, true === 1);
console.log(o1 === null, o1 === undefined, null === 0, undefined === 0);
// The marker probe three.js writes thousands of times a frame.
console.log(o1.isMesh === true, o1.isMesh === undefined, arr.isLight === true);

// --- functions ---------------------------------------------------------------
function fnA() {}
function fnB() {}
const fnC = fnA;
console.log(fnA === fnA, fnA === fnC, fnA === fnB, fnA === undefined);

// --- through a switch, which is `===` by another name -----------------------
function classify(v) {
    switch (v) {
        case 0:
            return 'zero';
        case -0:
            return 'negzero-unreachable';
        case 'material':
            return 'str';
        case 10n:
            return 'big';
        case null:
            return 'null';
        case undefined:
            return 'undef';
        default:
            return 'other';
    }
}
console.log(classify(0), classify(-0), classify('material'), classify(10n));
console.log(classify(null), classify(undefined), classify(NaN), classify(1));

// --- warmed, because a path that is never taken twice is not a path ---------
let hits = 0;
for (let i = 0; i < 500; i = i + 1) {
    if (nan === nan) hits = hits + 1000000;
    if (posZero === negZero) hits = hits + 1;
    if (litA === joinedA) hits = hits + 10;
    if (o1 === o3) hits = hits + 100;
    if (o1 === o2) hits = hits + 1000000;
    if (s1 === s2) hits = hits + 1000000;
    if (bigA === bigC) hits = hits + 1000;
}
console.log('hits', hits);

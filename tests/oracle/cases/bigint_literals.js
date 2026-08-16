// ECMA-262 12.9.3 BigIntLiteral and 6.1.6.2: the literal grammar, the type
// name, and how a BigInt prints. Every value here is exact and none of them
// is representable as a double, which is the point.
console.log(typeof 10n, typeof 10, typeof BigInt(1));
console.log(0n, 1n, 10n, 1234567890123456789012345678901234567890n);
console.log(0xffn, 0o777n, 0b1010n, 1_000_000n);
console.log(9007199254740993n, 9007199254740993n === 9007199254740993n);
console.log(2n ** 64n, 2n ** 128n);
console.log(-0n, 0n === -0n, Object.is(0n, -0n));

// A literal is a VALUE, not a shared object: it survives every place a value
// can be put, and equality follows the value rather than the identity.
const a = 12345678901234567890n;
const arr = [a, a * 2n];
const obj = { k: a };
const mk = (v) => () => v;
console.log(arr[0], arr[1], obj.k, mk(a)());
console.log(arr[0] === a, obj.k === a, mk(a)() === a);

// 6.1.6.2's ToString is the decimal digits with NO suffix; console.log's
// inspect format adds the `n` so output distinguishes 10n from 10.
console.log(String(255n), `${255n}`, "" + 255n);
console.log([1n, 2n], { v: 3n }, new Map([[4n, 5n]]), new Set([6n]));

// typeof answers "bigint" through every container and both inference modes.
function typeOfIt(x) { return typeof x; }
console.log(typeOfIt(1n), typeOfIt(arr[0]), typeOfIt(obj.k));

// The intrinsic surface 21.2 defines: a `BigInt` global that is also a
// property of the global object, an ordinary `BigInt.prototype` a primitive
// reaches by the ordinary walk, and 21.2.3.5's @@toStringTag.
console.log(typeof BigInt, typeof globalThis.BigInt, globalThis.BigInt === BigInt);
console.log(typeof BigInt.prototype, BigInt.prototype.constructor === BigInt);
console.log(Object.prototype.toString.call(1n), (1n).constructor === BigInt);
console.log(1n instanceof Object, typeof BigInt.asIntN, typeof BigInt.asUintN);
console.log(typeof (1n).toString, typeof (1n).valueOf, (1n).nope);

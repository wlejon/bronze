// The parts of spread whose answers are not obvious (docs/0017 decision 3).
//
// A string spreads by CODE POINT, not by UTF-16 code unit: the emoji below
// is one element of the spread and two units of `.length`, and 3 != 4 is the
// whole claim. It is the same walk for-of does (docs/0012 decision 2).
//
// An empty spread contributes nothing and is not a hole — `[0, ...[], 1]`
// has length 2 and no undefined in it. Evaluation is strictly left to right
// across a mixed list, which `order` records: the spread's own elements are
// evaluated when the array being spread is built, before the elements after
// it.
//
// A spread copy is FRESH but SHALLOW. Pushing to the copy leaves the
// original's length alone; mutating an object that was copied into it is
// visible through the original, because what was copied is the reference.
//
// Object spread copies own enumerable properties in enumeration order and a
// later key wins — but an EXISTING key keeps its position while taking the
// new value, which is why `{ b: 3, ...{ a: 1, b: 2 } }` prints b before a.
// Spreading null or undefined is a no-op (ECMA-262 7.3.25 CopyDataProperties
// returns early), not an error, and spreading an array copies its indices as
// the string keys they are.
console.log([..."abc"].length);
const emoji = "a\u{1F600}b";
console.log(emoji.length);
console.log([...emoji].length);
console.log([...[]].length);
console.log([0, ...[], 1]);
const order = [];
function t(v) { order.push(v); return v; }
const built = [t(1), ...[t(2), t(3)], t(4)];
console.log(order.join(""));
console.log(built.length);
function sum4(a, b, c, d) { return a + b + c + d; }
console.log(sum4(...[1, 2], 3, 4));
console.log(sum4(0, ...[1, 2, 3]));
const src = [1, 2];
const copy = [...src];
copy.push(3);
console.log(src.length + ":" + copy.length);
const shared = { v: 1 };
const wrapper = [...[shared]];
wrapper[0].v = 2;
console.log(shared.v);
console.log({ ...{ a: 1, b: 2 }, b: 3 });
console.log({ b: 3, ...{ a: 1, b: 2 } });
console.log({ ...null });
console.log({ ...undefined });
console.log({ ...[7, 8] });
console.log({ ...{}, ...{ z: 1 } });

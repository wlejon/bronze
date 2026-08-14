const arr = [10, 20, 30, 40, 50];
console.log(arr.at(0));
console.log(arr.at(-1));
console.log(arr.at(10));

const nested = [1, [2, [3, [4]]]];
console.log(nested.flat().length);
console.log(nested.flat(2).length);
console.log(nested.flat(Infinity).join(","));

const words = ["hello", "world"];
const flatMapped = words.flatMap(function (w) { return [w, w.toUpperCase()]; });
console.log(flatMapped.join(" "));

const numbers = [5, 12, 50, 130, 44];
console.log(numbers.findLast(function (x) { return x > 45; }));
console.log(numbers.findLastIndex(function (x) { return x > 45; }));

const orig = [3, 1, 4, 1, 5];
console.log(orig.toSorted().join(","));
console.log(orig.toReversed().join(","));
console.log(orig.toSpliced(1, 2, 99).join(","));
console.log(orig.with(2, 42).join(","));
console.log(orig.join(","));

const cw = [1, 2, 3, 4, 5];
cw.copyWithin(0, 3, 4);
console.log(cw.join(","));

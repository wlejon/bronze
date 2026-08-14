console.log(typeof Symbol.hasInstance);
console.log(typeof Symbol.species);
console.log(typeof Symbol.isConcatSpreadable);

const custom = {};
custom[Symbol.hasInstance] = function (inst) {
    return inst === 42;
};
console.log(42 instanceof custom);
console.log(43 instanceof custom);

const arr1 = [1, 2];
arr1[Symbol.isConcatSpreadable] = false;
const res1 = [0].concat(arr1);
console.log(res1.length);
console.log(res1[0]);
console.log(res1[1][0]);

const fakeArr = { 0: "a", 1: "b", length: 2 };
fakeArr[Symbol.isConcatSpreadable] = true;
const res2 = ["start"].concat(fakeArr);
console.log(res2.join(","));

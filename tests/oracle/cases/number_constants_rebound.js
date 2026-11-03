// The other side of number_constants_fold: a program that rebinds the global
// name `Number` gets no fold, so its reads see whatever the name now holds.

function eps() { return Number.EPSILON; }
console.log(eps() === 2 ** -52);

const original = Number;
globalThis.Number = { EPSILON: "replaced", MAX_SAFE_INTEGER: 1 };
console.log(eps(), Number.MAX_SAFE_INTEGER);

globalThis.Number = original;
console.log(eps() === 2 ** -52, Number.MAX_SAFE_INTEGER);

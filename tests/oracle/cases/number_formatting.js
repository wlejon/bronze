// JS number printing: full decimal inside [1e-6, 1e21), scientific
// outside, NaN/Infinity spelled out, and console.log's -0 (which ToString
// does not produce). Expressions avoid constructs not yet lowerable
// (unary minus, exponent literals, global NaN/Infinity bindings).
console.log(42);
console.log(3000000);
console.log(0.5);
console.log(123.456);
console.log(0.000001);
console.log(1 / 10000000);
console.log(0.1 + 0.2);
console.log(1 / 3);
console.log(0 / 0);
console.log(1 / 0);
console.log((0 - 1) / 0);
console.log(0 * (0 - 1));
console.log(1000000000000 * 1000000000);

// Number to string for integral values, through String(), template literals,
// property keys and Array.prototype.join: below 2^53 the digits are the exact
// integer's; at and past it the shortest round-trip digits padded with zeros,
// and from 1e21 the exponent form.
const values = [
    0, -0, 1, -1, 7, 10, 99, 100, 1001, 1023, 1009, 3000000, 65535, 2147483647, 2147483648,
    4294967295, 4294967296, -4294967297, 1e15, 123456789012345, 999999999999999,
    9007199254740990, 9007199254740991, -9007199254740991, 9007199254740992, 9007199254740993,
    9007199254740994, 2 ** 60, -(2 ** 60), 2 ** 63, 2 ** 64, 2 ** 64 + 4096, 1e20, 123456789012345680000,
    999999999999999900000, 1e21, -1e21, 1.5e300, 2 ** 70, 0.5, -0.5, 1.5, 1e-7, 4.35, NaN, Infinity, -Infinity,
];
for (const v of values) console.log(String(v), `${v}`, v + '');
console.log(values.join());
console.log(values.join(', '));
console.log(values.join(undefined));
console.log([1, 'a', true, false, null, undefined, 2.5, [3, [4, 5]], { toString() { return 'o'; } }].join());
console.log([].join(), [7].join(), [, , 3].join(), new Array(3).join('-'));
const o = {};
o[1009] = 'a';
o[9007199254740993] = 'b';
o[2 ** 64] = 'c';
console.log(Object.keys(o).join('|'));
// join with a separator whose toString runs, and a sparse array
let calls = 0;
console.log([1, 2, 3].join({ toString() { calls++; return '+'; } }), calls);
console.log(Array.prototype.join.call({ length: 3, 0: 1023, 2: 2 ** 53 }));
console.log(Array.prototype.join.call('abc'));

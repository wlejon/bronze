// Math is not a variable: it is a compile-time-known namespace resolved by
// name, the same shape of problem `Object.keys` solved. Every expectation here
// is ECMA-262, and the irrational ones are pinned at full shortest-round-trip
// precision so a float-vs-double implementation cannot pass by rounding.
console.log(Math.abs(-5));
console.log(Math.max(1, 7, 3));
console.log(Math.min(1, 7, 3));
console.log(Math.floor(2.7));
console.log(Math.ceil(2.1));
console.log(Math.round(2.5));
console.log(Math.round(-2.5));
console.log(Math.trunc(-2.7));
console.log(Math.sqrt(9));
console.log(Math.sqrt(2));
console.log(Math.sign(-3));
console.log(Math.pow(2, 10));
console.log(Math.hypot(3, 4));
console.log(Math.PI);
console.log(Math.E);

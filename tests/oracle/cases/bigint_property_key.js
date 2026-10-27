// A BigInt as a property key (7.1.19 ToPropertyKey): ToString(1n) is "1", so
// o[1n] is o["1"] and a[1n] is a[1]. A computed BigInt key was a fatal
// "computed property key that is still an object" before.
const o = {};
o[1n] = "one";
console.log(o["1"], o[1n], 1n in o, Object.keys(o).join());
o[-5n] = "neg";
console.log(o["-5"], Object.keys(o).join());

const a = [10, 20, 30];
console.log(a[1n], a[3n], a[-1n]);
a[2n] = 33;
console.log(a.join(), a.length);

const big = 123456789012345678901234567890n;
o[big] = "big";
console.log(o["123456789012345678901234567890"]);
delete o[1n];
console.log(1n in o, Object.keys(o).join());

const ta = new Int32Array([7, 8]);
console.log(ta[1n]);

const m = { [2n]: "computed" };
console.log(m[2], Object.keys(m).join());

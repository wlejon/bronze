// Number.prototype.toString(radix) over integers, fractions and the edges.
const ints = [0, -0, 1, -1, 7, 8, 9, 10, 15, 16, 35, 36, 37, 255, 256, 1023, 1024, 65535, 0xffffff, 0x7fffffff,
    0x80000000, 0xffffffff, 0x100000000, 2 ** 52, 2 ** 53 - 1, -(2 ** 53 - 1)];
for (const radix of [2, 3, 8, 10, 16, 32, 36]) {
    console.log(radix, ints.map((n) => n.toString(radix)).join(' '));
}
// Past the safe integers every digit is still exact in a power-of-two radix.
const big = [2 ** 53, 2 ** 53 + 2, -(2 ** 53), 2 ** 64, 2 ** 80 + 2 ** 30, -(2 ** 100)];
for (const radix of [2, 8, 16, 32]) {
    console.log(radix, big.map((n) => n.toString(radix)).join(' '));
}
console.log((2 ** 53).toString(10), (2 ** 53 + 2).toString(), (1e21).toString(10), (-1e21).toString());
const fractions = [0.5, -0.5, 0.1, 1.5, 255.5, 1 / 3, 1e-7, 123.456, -0.000001, 2 ** -20, 4294967295.5];
for (const radix of [2, 16, 36]) {
    console.log(radix, fractions.map((n) => n.toString(radix)).join(' '));
}
console.log(NaN.toString(16), Infinity.toString(2), (-Infinity).toString(36));
console.log((255).toString(), (255).toString(undefined), (255).toString(10), (255).toString('16'), (255).toString(16.9));
for (const bad of [0, 1, 37, -2, NaN, Infinity]) {
    try { (1).toString(bad); console.log('no throw', bad); } catch (e) { console.log(e instanceof RangeError, bad); }
}
// Round trip through parseInt.
let ok = true;
for (let i = -3000; i < 3000; i += 7) {
    for (const radix of [2, 7, 16, 36]) if (parseInt(i.toString(radix), radix) !== i) ok = false;
}
console.log(ok);
// The hex colour strings pixi builds.
const hex = (n) => '#' + (n | 0x1000000).toString(16).slice(1);
console.log(hex(0), hex(0xff), hex(0xabcdef), hex(0xffffff), (0xff0000).toString(16).padStart(6, '0'));
console.log(new Number(254).toString(16), Number.prototype.toString.call(254, 2));
try { Number.prototype.toString.call('254', 16); } catch (e) { console.log(e instanceof TypeError); }

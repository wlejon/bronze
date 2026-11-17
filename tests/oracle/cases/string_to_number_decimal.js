// ToNumber of a string: the short decimal spellings the runtime reads in place
// (sign, up to 15 digits, one point) and the neighbours that must take the
// full StringNumericLiteral grammar instead.
const inputs = [
    "0", "-0", "+0", "7", "007", "-12", "+12", "12.5", "-12.5000", "1.", ".5",
    "+.5", "-.5", ".", "+", "-", "", "1..2", "1.2.3", "--1", "+-1",
    "123456789012345", "-123456789012345", "1234567890123456",
    "12345678901234567", "0.1", "0.3", "1.7976931348623157", "9007199254740993",
    "0.000001", "123.456789012345", "99999999999999.9", "1e3", "1E-2", " 12",
    "12 ", "\t3\n", "0x1f", "0b101", "0o17", "-0x1", "Infinity", "-Infinity",
    "12px", "1_000", "١٢", " 12", "12 ", "3.14abc",
];
for (const s of inputs) {
    const n = Number(s);
    const neg0 = n === 0 && 1 / n < 0;
    console.log(JSON.stringify(s), n, neg0 ? "-0" : "", +s === n || (n !== n && +s !== +s), s * 1 === n || n !== n);
}
// From a match, as a font size is read.
const m = /(\d+(?:\.\d+)?)px/.exec("bold 18.75px sans-serif");
console.log(Number(m[1]), Number(m[1]) * 2);
// A string built at run time, then converted in a loop.
let sum = 0;
for (let i = 0; i < 2000; i++) sum += Number(String(i % 97) + "." + String(i % 7));
console.log(sum.toFixed(6));

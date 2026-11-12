// Numbers turned into strings: small integers, their neighbours, and
// everything that is not one.
const nums = [0, -0, 1, 9, 10, 99, 100, 999, 1000, 1022, 1023, 1024, 1025, 4095, -1, -1023, 1.5, 0.1, 1023.5,
    1e21, 1e-7, NaN, Infinity, -Infinity, 2 ** 31, 2 ** 32, 2 ** 53];
console.log(nums.map((n) => String(n)).join(' '));
console.log(nums.map((n) => `${n}`).join(' '));
console.log(nums.map((n) => '' + n).join(' '));
console.log(nums.map((n) => n + 'px').join(' '));
console.log(nums.map((n) => n.toString()).join(' '));
console.log(nums.map((n) => [n].join()).join(' '));

// Every small integer, both ways, and each result is an ordinary string.
let ok = true;
for (let i = 0; i < 1100; i++) {
    const s = String(i), t = `${i}`;
    if (s !== t || Number(s) !== i || s.length !== (i < 10 ? 1 : i < 100 ? 2 : i < 1000 ? 3 : 4)) ok = false;
    if (s + 'x' !== i + 'x' || s.charCodeAt(s.length - 1) !== 48 + (i % 10)) ok = false;
}
console.log(ok);

// As property keys and in keys built by concatenation.
const obj = {};
for (let i = 0; i < 1100; i += 50) obj[i] = i;
console.log(Object.keys(obj).join(), obj['1050'], obj[String(1000)], obj[`${50}`]);
const key = (size, weight, family) => `${size}-${weight}-${family}`;
console.log(key(26, 400, 'Arial'), key(0, 1023, 1024), key(-0, 1.5, null), key(undefined, true, 7));

// Other primitives through the same conversion.
console.log(String(true), `${false}`, '' + null, `${undefined}`, String('s'), `${10n}`, String(Symbol('d')));
try { `${Symbol('d')}`; } catch (e) { console.log(e instanceof TypeError); }
console.log(String({ toString() { return 5; } }), `${[1, [2, 3]]}`, '' + { valueOf() { return 1023; } });

// The strings are values, not shared mutable things.
const a = String(7), b = String(7);
console.log(a === b, typeof a, a.length, Object(a) === Object(b), (a + b), a.padStart(3, '0'));

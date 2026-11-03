// `x.length` with nothing known about `x`: the inline String arm (the
// header's code-unit count) beside the Array and typed-array arms, and every
// other receiver at the same site.

function len(x) { return x.length; }

let built = "";
for (let i = 0; i < 40; i++) built += String.fromCharCode(97 + (i % 26));
const receivers = [
  "", "a", "hello", "héllo", "\u{1F600}", "a\u{1F600}b\uD800", built, built + built,
  "abcdef".slice(1, 4), String(12345), `t${1 + 1}t`, Object.keys({ key1: 1 })[0],
  [1, 2, 3], new Float32Array(7), { length: 9 }, { length: "str" }, function (a, b) {},
  new String("boxed"), Object("xy"), [], { }
];
// Warm one site with every receiver kind, several rounds.
let total = 0;
for (let r = 0; r < 200; r++) for (const x of receivers) { const l = len(x); total += typeof l === "number" ? l : 0; }
console.log("total", total);
console.log(receivers.map(len).join(","));

// A builder string: the `+=` chain's accumulator answers its current length.
let acc = "";
const seen = [];
for (let i = 0; i < 5; i++) { acc += "ab"; seen.push(len(acc)); }
console.log("builder", seen.join(","));

// Primitives without a string arm.
for (const p of [1, true, Symbol("s"), 10n]) console.log(typeof p, len(p));
for (const p of [undefined, null]) {
  try { len(p); } catch (e) { console.log("nullish", e.constructor.name); }
}

// String.prototype cannot change a string's own length.
String.prototype.length = 99;
Object.defineProperty(String.prototype, "extra", { get() { return this.length; } });
console.log("proto", len("abc"), "abcd".extra);

// Strings created and dropped across collections.
let sum = 0;
for (let i = 0; i < 20000; i++) {
  const s = "k" + i + "-" + "x".repeat(i % 7);
  const junk = [{ s }, [s, s]];
  sum += len(s) + junk.length;
}
console.log("gc", sum);

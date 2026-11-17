// A slice covering the whole string answers that string's text: split with no
// separator found, a match of the whole input, substring/slice of the full
// range, and the same through a `+`-chain string still growing.
const NL = /(?:\r\n|\r|\n)/;
const words = ["alpha", "bravo charlie", "", "x", "line one\nline two\r\nthree"];
for (const w of words) {
    const parts = w.split(NL);
    console.log(parts.length, JSON.stringify(parts), parts[0] === w, parts[0].length);
}
// The pieces array grows past its first block.
const many = Array.from({ length: 30 }, (_, i) => "p" + i).join("\n");
const pieces = many.split(NL);
pieces.push("tail");
console.log(pieces.length, pieces[0], pieces[29], pieces[30]);
const withCaps = "a1b22c333d".split(/(\d+)/);
console.log(JSON.stringify(withCaps), withCaps.length);
console.log(JSON.stringify("abc".split(/x/, 0)), JSON.stringify("".split(/x/)), JSON.stringify("".split(/(?:)/)));
// A match of the whole input.
const whole = /^.*$/.exec("entire input");
console.log(whole[0], whole[0] === whole.input, whole.index);
// A string under construction: the full-range slice must not alias its slack.
let s = "start";
for (let i = 0; i < 5; i++) s += "-" + i;
const full = s.split(/\|/)[0];
s += "!";
console.log(full, s, full.length, s.length);
let t = "";
for (let i = 0; i < 40; i++) t += String.fromCharCode(97 + (i % 26));
const copy = t.substring(0, t.length);
const sl = t.slice(0);
t += "Z";
console.log(copy, sl, t, copy === sl);
const u = "héllo wörld ✓";
console.log(u.split(/;/)[0] === u, u.substring(0, u.length), /.*/u.exec(u)[0]);

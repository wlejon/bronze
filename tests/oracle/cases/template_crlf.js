const s = `a
b`;
console.log(s.length);
console.log(s === "a\nb");
function tag(strings) { return strings.raw[0]; }
const r = tag`x
y`;
console.log(r.length);
console.log(r === "x\ny");
const c = "p\
q";
console.log(c);

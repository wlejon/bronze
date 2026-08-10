function step(x) {
  const a = x * 1.000001 + 0.5;
  const b = a * 0.999999 - 0.25;
  return b * 1.000002;
}
function loop10(x) {
  const x1 = step(x);
  const x2 = step(x1);
  const x3 = step(x2);
  const x4 = step(x3);
  const x5 = step(x4);
  const x6 = step(x5);
  const x7 = step(x6);
  const x8 = step(x7);
  const x9 = step(x8);
  const x10 = step(x9);
  return x10;
}
function loop100(x) {
  const y1 = loop10(x);
  const y2 = loop10(y1);
  const y3 = loop10(y2);
  const y4 = loop10(y3);
  const y5 = loop10(y4);
  const y6 = loop10(y5);
  const y7 = loop10(y6);
  const y8 = loop10(y7);
  const y9 = loop10(y8);
  const y10 = loop10(y9);
  return y10;
}
function loop1000(x) {
  const z1 = loop100(x);
  const z2 = loop100(z1);
  const z3 = loop100(z2);
  const z4 = loop100(z3);
  const z5 = loop100(z4);
  const z6 = loop100(z5);
  const z7 = loop100(z6);
  const z8 = loop100(z7);
  const z9 = loop100(z8);
  const z10 = loop100(z9);
  return z10;
}
function loop10000(x) {
  const w1 = loop1000(x);
  const w2 = loop1000(w1);
  const w3 = loop1000(w2);
  const w4 = loop1000(w3);
  const w5 = loop1000(w4);
  const w6 = loop1000(w5);
  const w7 = loop1000(w6);
  const w8 = loop1000(w7);
  const w9 = loop1000(w8);
  const w10 = loop1000(w9);
  return w10;
}
console.log(loop10000(1.0));

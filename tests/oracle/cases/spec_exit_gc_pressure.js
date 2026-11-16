// Exits with many values live across them while the loop allocates: the
// first call reaches a constructor whose fields' cells are not filled yet
// only late in its loop, after enough garbage that the collector has run at
// the safepoints before it, so the failed guard hands the twin doubles and
// objects that were live across those collections.
class Snap {
  constructor(a, b, list) { this.a = a; this.b = b; this.list = list; }
}
function work(items, scale) {
  let a = 0.5, b = 1.5, c = 2.5, d = 3.5, e = 4.5, f = 5.5, g = 6.5, h = 7.5;
  let keep = [], junk = null, snap = null;
  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    const ov = it.v * scale;
    const o = { v: ov, w: [it.v, i] };
    keep.push(o);
    if (keep.length > 16) keep = [keep[keep.length - 1]];
    for (let z = 0; z < 40; z++) junk = [z, a, o, junk && junk[0]];
    a += ov; b -= ov * 0.5; c *= 1.0001; d += it.v / (i + 1);
    e = e * 0.99 + it.v; f += a - b; g = g + c - d; h += e * 0.001;
    if (i === items.length - 7) {
      snap = new Snap(a + b, [c, d, e], keep);
      const at = { a, b, c, d, e, f, g, h };
      h += at.a * 0 + at.h * 0;
    }
  }
  return [a, b, c, d, e, f, g, h, keep.length, keep[0].w[1], snap.a, snap.b[2], snap.list.length, junk[0]]
    .map(x => +x.toFixed(6)).join(' ');
}
const items = [];
for (let i = 0; i < 400; i++) items.push({ v: (i % 17) * 0.5 });
console.log(work(items, 1.25));
const late = items.slice();
late[150] = { v: '3' };
late[170] = { v: -0 };
console.log(work(late, 2));
let s = 0;
for (let r = 0; r < 300; r++) s += work(items, 1 + (r & 3)).length;
console.log('warm', s);
console.log(work(late, 0.5));

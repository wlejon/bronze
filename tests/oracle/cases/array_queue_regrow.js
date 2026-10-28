// An array used as a queue (push at the end, shift at the front) leaves room
// at its front; a write past the end either moves the elements down into it
// or grows the block (runtime/array.cpp, setElemSlow). Whichever it does, the
// array reads the same: these pin the contents across queues that stay
// small, stay nearly full, grow and drain, and take writes past the end
// (leaving holes) right after shifts. Each element is a fresh object, for
// the GC-stress re-run. And a loop reading `a[0]` across the methods that
// move elements must read it afresh each time round (the lowering's loop
// hoisting once took push/shift/sort/splice/fill/copyWithin as writing only
// `length`, and moved the read out of the loop).

function queue(steady, rounds) {
  const q = [];
  let out = 0, seen = "";
  for (let i = 0; i < rounds; i++) {
    q.push({ v: i });
    if (q.length > steady) {
      const x = q.shift();
      out += x.v;
      if (i % 97 === 0) seen += x.v + ",";
    }
  }
  return out + " " + q.length + " " + q[0].v + " " + q[q.length - 1].v + " " + seen;
}
for (const steady of [1, 3, 7, 8, 9, 15, 16, 17, 63, 64, 65]) console.log(steady, queue(steady, 3000));

// Grow, then drain to empty, then grow again.
const g = [];
for (let r = 0; r < 3; r++) {
  for (let i = 0; i < 100; i++) g.push({ v: r * 1000 + i });
  let s = 0;
  while (g.length > 10 * r) s += g.shift().v;
  console.log("drain", r, s, g.length, g.length ? g[0].v : "-");
}

// A write past the end after shifts: the gap reads as holes.
const h = [];
for (let i = 0; i < 12; i++) h.push(i);
for (let i = 0; i < 5; i++) h.shift();
h[h.length + 3] = "far";
console.log(h.length, JSON.stringify(h), 9 in h, 10 in h, h.indexOf("far"));
for (let i = 0; i < 4; i++) h.shift();
h.push("p");
h[20] = "z";
console.log(h.length, JSON.stringify(h), Object.keys(h).join(","));

// A loop that reads `a[0]` and calls the methods that move the elements: the
// read is not loop-invariant (each method writes what it reads).
const m = [];
let firsts = "";
for (let r = 0; r < 4; r++) {
  m.push({ v: r * 10 });
  firsts += m[0].v + ":" + m[m.length - 1].v + " ";
  if (r % 2) m.shift(); else m.unshift({ v: -r });
}
const rv = [3, 1, 2];
let rs = "";
for (let r = 0; r < 3; r++) { rs += rv[0] + ","; if (r === 0) rv.sort(); else rv.reverse(); }
const sp = [1, 2, 3, 4];
let ss = "";
for (let r = 0; r < 3; r++) { ss += sp[1] + ","; sp.splice(1, 1); }
const fl = [0, 0, 0];
let fs = "";
for (let r = 0; r < 3; r++) { fs += fl[2] + ","; fl.fill(r + 1); }
const cw = [1, 2, 3, 4, 5];
let cs = "";
for (let r = 0; r < 3; r++) { cs += cw[0] + ","; cw.copyWithin(0, 1); }
console.log(firsts.trim(), "|", rs, ss, fs, cs);

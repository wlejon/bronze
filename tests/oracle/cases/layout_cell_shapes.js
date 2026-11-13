// Layout cells (opt/spec/feedback_plan.h applyFeedbackLayoutCells, runtime
// static_shape.cpp bronze_static_layout_publish): the sites of a hot function
// that the harvest saw with one shape share one cell, published by the first
// object that has the whole layout. Here the objects that reach those sites
// are not all of that shape: the same keys in another order, a prefix of the
// layout and an extension of it, a frozen object and a store, an accessor in
// place of a slot, a deleted and re-added key, a dictionary object, a
// prototype object, and in `late` the wrong shapes arrive first. The
// harvest suite's one-way legs give every site of these functions one of the
// shapes only, so each of the others is a miss there.

class P { constructor(x, y) { this.x = x; this.y = y; this.n = 0; } }

function move(p, dx, dy) {
  p.x = p.x + dx;
  p.y = p.y + dy;
  p.n = p.n + 1;
  return p.x * 3 + p.y * 5 + p.n;
}

function read(p) {
  const a = p.x, b = p.y;
  return a + b * 2 + p.x - p.y + (p.n | 0);
}

function late(p, k) {
  p.x = k;
  const v = p.y + p.x;
  p.y = v % 97;
  return v + p.x;
}

function make(kind, i) {
  switch (kind) {
    case 0: return new P(i, i + 1);
    case 1: return { y: i + 1, x: i, n: 0 };                 // other order
    case 2: return { x: i, y: i + 1 };                       // prefix
    case 3: { const p = new P(i, i + 1); p.z = 7; return p; } // extension
    case 4: return Object.freeze(new P(i, i + 1));
    case 5: {
      const p = { x: i, n: 0 };
      let y = i + 1;
      Object.defineProperty(p, "y", { get() { return y; }, set(v) { y = v + 1; }, enumerable: true });
      return p;
    }
    case 6: { const p = new P(i, i + 1); delete p.x; p.x = i; return p; }
    case 7: {
      const p = new P(i, i + 1);
      for (let k = 0; k < 40; k++) p["k" + k] = k;
      for (let k = 0; k < 40; k += 2) delete p["k" + k];
      return p;
    }
    case 8: { const p = new P(i, i + 1); Object.create(p); return p; }
    case 9: { const p = new P(i, i + 1); Object.defineProperty(p, "y", { writable: false }); return p; }
    default: return new P(i, i + 1);
  }
}

let sum = 0;
function note(v) { sum = (sum + (v | 0)) % 1000000007; }

// `late` meets every odd shape before it meets a P.
for (let kind = 9; kind >= 0; kind--) {
  for (let i = 0; i < 300; i++) {
    const p = make(kind, i);
    note(late(p, i & 15));
    note(read(p));
  }
}
console.log(sum);

// `move` and `read` are warm on P, then meet the others one in a few.
const ps = [];
for (let i = 0; i < 64; i++) ps.push(new P(i, i + 1));
for (let r = 0; r < 3000; r++) {
  const p = ps[r & 63];
  note(move(p, 1, 2));
  note(read(p));
}
console.log(sum);
for (let r = 0; r < 6000; r++) {
  const kind = r % 7 === 0 ? (r / 7) % 10 : 0;
  const p = kind ? make(kind, r & 31) : ps[r & 63];
  note(move(p, r & 3, 1));
  note(read(p));
  note(late(p, r & 7));
}
console.log(sum);

// Every call a different shape.
for (let r = 0; r < 4000; r++) {
  const p = make(r % 10, r & 31);
  note(move(p, 2, r & 1));
  note(read(p));
}
console.log(sum);
const f = make(4, 3), g = make(9, 3), h = make(5, 3);
console.log(move(f, 1, 1), f.x, f.y, f.n, move(g, 1, 1), g.x, g.y, g.n, move(h, 1, 1), h.x, h.y, h.n);

// Temporary objects constructed and read inside a hot loop stay virtual on
// hot paths in S and materialize lazily only on a cold `leave` guard exit to G.
class Pt {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
}

function stepWithColdGuard(n, gate) {
  let sum = 0;
  for (let i = 0; i < n; i = i + 1) {
    const p = new Pt(i, i + 1);
    const pair = { a: p.x, b: p.y };
    // Reading gate.tag guards on the dominant shape of `gate`, keeping `p`
    // and `pair` live in the exit state of the cold `leave` block.
    sum += gate.tag + p.x + p.y + pair.b - pair.a;
  }
  return sum;
}

const hotGate = { tag: 10 };
const coldGate = { extra: 1, tag: 25 };

let acc = 0;
for (let r = 0; r < 500; r++) {
  acc += stepWithColdGuard(10, hotGate);
}
// Triggers the shape guard exit mid-loop after allocations were kept virtual.
acc += stepWithColdGuard(5, coldGate);
console.log(acc);

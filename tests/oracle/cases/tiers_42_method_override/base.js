// A base class whose inherited method calls another of its own methods on
// `this`: lowering names the base's declaration as the callee, and a subclass
// in the importing module overrides it.
class Cell {
  constructor(n) { this.n = n; }
}

export class Layer {
  constructor() {
    this.cells = new Map();
    this.calls = 0;
  }

  keyOf(x) {
    return [x >> 2, x & 3];
  }

  batch(x) {
    const [a, b] = this.keyOf(x);
    return this.batchAt(a, b);
  }

  batchAt(a, b) {
    const k = a * 1024 + b;
    let c = this.cells.get(k);
    if (!c) { c = new Cell(k); this.cells.set(k, c); }
    this.calls++;
    return c;
  }
}

export class Counter {
  constructor() { this.k = 1; }
  tick(o) { return this.k; }
  run(n) {
    const o = { x: 0 };
    let s = 0;
    for (let i = 0; i < n; i++) {
      this.tick(o);
      s += o.x;
    }
    return s;
  }
}

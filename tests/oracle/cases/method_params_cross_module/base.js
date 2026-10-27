// The base class, in a module of its own: its subclasses name it through an
// import, which the module graph's flattening renames.
export class Base {
  constructor() { this.k = 1; }
  m(x) { return x + this.k; }
  twice(x) { return this.m(x) + this.m(x); }
  describe(v) { return 'base:' + v; }
}

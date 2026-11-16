// `const had = !!table[k]` computed before speculated sites and branched on
// after them: the branch reads the double negation, which the backend lowers
// as a branch on the inner compare with its targets swapped. The inner
// compare is defined before the resume points between them, so the branch
// must read the negation itself, which is in their state.
class Store {
  constructor() { this.id = { uid: 3 }; this.made = 0; }
  make(ctx) { this.made++; const g = { n: 0, reset() { this.n = 0; } }; ctx.data[this.id.uid] = g; return g; }
  update(ctx) {
    const had = !!ctx.data[this.id.uid];
    const g = ctx.data[this.id.uid] || this.make(ctx);
    if (ctx.dirty || !had) {
      if (had) g.reset();
      g.n += ctx.w;
      ctx.dirty = false;
    }
    return g;
  }
}
function main() {
  const s = new Store();
  let total = 0;
  for (let i = 0; i < 20000; i++) {
    const ctx = { data: [], dirty: i % 3 === 0, w: i % 7 };
    total += s.update(ctx).n;
    ctx.dirty = true;
    total += s.update(ctx).n;
  }
  console.log(total, s.made);
}
main();

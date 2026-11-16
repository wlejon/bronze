// Error.stack through calls the optimiser inlines two deep, and through a
// speculated `new` whose constructor it inlines: every frame is named, in
// order, before and after a guard of the inlined code starts failing.
function frames(stack) {
  const m = stack.match(/at (?:new )?([A-Za-z_$][\w$.]*)/g) || [];
  return m.map(s => s.replace(/^at (new )?/, "").replace(/^.*\./, "")).slice(0, 3).join(">");
}
function inner(x) { if (x.n === 13) throw new Error("inner"); return x.n; }
function mid(x) { return inner(x) + 1; }
function top(x) { return mid(x) * 2; }
class K { constructor(v) { if (v === 13) throw new TypeError("ctor"); this.v = v; } }
function make(v) { return new K(v).v; }
function main() {
  const seen = new Set();
  let s = 0;
  for (let i = 0; i < 30000; i++) {
    const x = i < 20000 ? { n: i % 20 } : { m: 0, n: i % 20 };
    try { s += top(x); } catch (e) { seen.add(frames(e.stack)); }
    try { s += make(i % 20); } catch (e) { seen.add(frames(e.stack)); }
  }
  console.log(s, [...seen].join(" | "));
}
main();

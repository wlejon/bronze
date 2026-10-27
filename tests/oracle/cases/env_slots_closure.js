// Captured bindings read and written through their records, at several
// depths, a TDZ read from a closure, and records holding objects across
// collections (the write barrier's case).
function counter() {
  let n = 0;
  const box = { items: [] };
  return {
    inc() { n++; return n; },
    nest() {
      let m = 10;
      return () => { m += n; n += 1; return m; };
    },
    keep(i) { box.items = box.items.concat([{ i, s: "v" + i }]); return box.items.length; },
    sum() { return box.items.reduce((a, o) => a + o.i, 0); },
  };
}
const c = counter();
c.inc(); c.inc();
const f = c.nest();
console.log(f(), f(), f(), c.inc());
for (let i = 0; i < 20000; i++) c.keep(i % 7);
console.log(c.keep(100), c.sum());

function early() {
  const read = () => late;
  try { read(); } catch (e) { console.log(e instanceof ReferenceError, e.message); }
  let late = 5;
  return read();
}
console.log(early());

let holder = null;
function makeHolders() {
  let obj = null;
  const set = () => { obj = { big: new Array(100).fill(1) }; };
  const get = () => obj.big.length;
  return { set, get };
}
const hs = [];
for (let i = 0; i < 3000; i++) { const h = makeHolders(); h.set(); hs.push(h); }
let total = 0;
for (const h of hs) total += h.get();
console.log(total, holder);

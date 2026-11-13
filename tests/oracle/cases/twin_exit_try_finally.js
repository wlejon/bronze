// Guards failing inside try, catch and finally blocks: the resume twin is
// entered inside the region, so the throw that follows is caught by the
// twin's own handler, the finally block runs once, and the values the
// handlers read are the ones the speculated code left.

class Box { constructor(v) { this.v = v; this.n = 0; } }
const out = [];

function guarded(box, other, i) {
  let stage = 0;
  let acc = 0;
  try {
    stage = 1;
    acc += box.v;
    acc += other.v;              // fails in the try
    stage = 2;
    if (i % 1500 === 1499) throw new Error('t' + i);
    acc += box.v * 2;
  } catch (e) {
    stage += 10;
    acc += other.n === undefined ? 1000 : other.n;   // fails in the catch
    out.push(e.message + ':' + stage);
  } finally {
    box.n += 1;
    acc += other.v === undefined ? -1 : 0;           // fails in the finally
    stage += 100;
  }
  return acc + stage;
}

function run(n, odd) {
  const box = new Box(2);
  const same = new Box(3);
  let s = 0;
  for (let i = 0; i < n; i++) s += guarded(box, i % 499 === 498 ? odd : same, i);
  return s + ':' + box.n;
}

console.log(run(6000, new Box(5)));
console.log(run(6000, { v: 7 }));
console.log(run(6000, { n: 4 }));
try { run(6000, { get v() { throw new Error('getter'); }, n: 1 }); } catch (e) { console.log('escaped', e.message); }
console.log(out.length, out.slice(0, 3).join(','), out[out.length - 1]);

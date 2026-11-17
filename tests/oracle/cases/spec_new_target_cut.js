// new.target read in a branch the harvested run never took. The constructor's
// optimistic copy (opt/speculate optimistic.h) leaves at that branch instead
// of holding it, so its own body reads no new.target; the call still finishes
// in the generic code, which does. The copy's function object must therefore
// not be entered without the runtime's construct scope (il.h
// functionEntersDirectly): a run that did not train meets negative inputs and
// reads new.target there, inside another constructor's construction too.
const TRAINING = Math.random() < 0.5;
if (TRAINING) console.error('TRAINING');

class Point {
  constructor(x) {
    if (x < 0) {
      seen.add('Point new.target ' + (new.target === Point) + ' ' + (new.target && new.target.name));
      x = -x;
    }
    this.x = x;
  }
}
class Outer {
  constructor(x) {
    this.p = new Point(x);
  }
}
const seen = new Set();

function main() {
  let s = 0;
  for (let i = 0; i < 2000000; i++) {
    const x = !TRAINING && i % 400000 === 5 ? -i : i & 15;
    s += new Point(x).x + new Outer(x).p.x;
  }
  console.log(s);
  for (const t of seen) console.log(t);
}
main();

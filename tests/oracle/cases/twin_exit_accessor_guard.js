// An inlined accessor's guard that leaves for the resume twin: the loop reads
// and writes `x` through a class's accessor pair, and now and then meets a
// receiver whose `x` is something else: an own data property, a subclass
// with its own accessor, an object with a getter only (the store is ignored
// in sloppy code), a frozen instance (the setter's store throws, mid-loop),
// a primitive.

class Pt {
  constructor(v) { this._x = v; this.hits = 0; }
  get x() { return this._x; }
  set x(v) { this._x = v; this.hits++; }
}
class Sub extends Pt {
  get x() { return this._x * 10; }
  set x(v) { this._x = v + 1; }
}

function step(list) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    const p = list[i];
    const was = p.x;
    p.x = was === undefined ? 0 : was + 1;
    const v = p.x;
    s += v === undefined ? -1 : v;
  }
  return s;
}

const list = [];
for (let i = 0; i < 200; i++) list.push(new Pt(i));
const own = { x: 5, _x: 1, hits: 0 };
const sub = new Sub(3);
const getOnly = { get x() { return 9; }, _x: 0, hits: 0 };
const frozen = Object.freeze(new Pt(11));
const odd = [own, sub, getOnly, frozen, 7, 'q'];

let total = 0;
let thrown = 0;
for (let r = 0; r < 600; r++) {
  if (r > 0 && r % 60 === 0) {
    const k = r % list.length;
    const keep = list[k];
    list[k] = odd[(r / 60 - 1) % odd.length];
    try {
      total += step(list);
    } catch (e) {
      thrown += e instanceof TypeError ? 1 : 100;
    }
    list[k] = keep;
  } else {
    total += step(list);
  }
}
console.log(total, thrown);
console.log(own.x, sub.x, sub._x, getOnly.x, frozen.x, list[0].hits, list[199].x);

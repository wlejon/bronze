// A hot accessor with a rare first-use path (`cache[k] || init(o)`, pixi's
// _getGpuSprite): the inliner keeps the rare call a call (opt/inline
// driver.cpp, RARE calls) while its hot callers take the accessor. The
// rare path must still run, return the right record and be seen by the hot
// path afterwards; late in the run new objects take it again (more often
// than the harvest saw), and one takes it with a getter that throws.
class Bounds {
  constructor() {
    this.minX = 0;
    this.maxX = 0;
  }
}
function fillBounds(b, item) {
  b.minX = -item.anchor * item.w;
  b.maxX = b.minX + item.w;
}
class Item {
  constructor(i) {
    this.w = 2 + (i % 7);
    this.anchor = (i % 3) * 0.5;
    this.data = {};
    this._bounds = new Bounds();
  }
  get bounds() {
    fillBounds(this._bounds, this);
    return this._bounds;
  }
}
class Record {
  constructor(item) {
    this.item = item;
    this.bounds = item.bounds;
    this.uses = 0;
  }
}
const pipe = {
  uid: 3,
  init(item) {
    const r = new Record(item);
    item.data[this.uid] = r;
    return r;
  },
  get(item) {
    return item.data[this.uid] || this.init(item);
  },
  update(item) {
    const r = this.get(item);
    r.uses = r.uses + 1;
    return r.bounds.maxX - r.bounds.minX;
  },
};
const items = [];
for (let i = 0; i < 200; i = i + 1) items.push(new Item(i));
let total = 0;
for (let frame = 0; frame < 300; frame = frame + 1) {
  for (let i = 0; i < items.length; i = i + 1) total = total + pipe.update(items[i]);
  if (frame > 250) items.push(new Item(frame * 7));
}
console.log(total);
console.log(items.length, items[0].data[3].uses, items[items.length - 2].data[3].uses, items[items.length - 1].data[3]);
const odd = new Item(5);
Object.defineProperty(odd, "bounds", {
  get() {
    throw new Error("no bounds " + this.w);
  },
});
try {
  pipe.update(odd);
} catch (e) {
  console.log(e.message, odd.data[3] === undefined);
}
odd.data[3] = { bounds: { minX: 1, maxX: 4.5 }, uses: 0 };
console.log(pipe.update(odd), odd.data[3].uses);

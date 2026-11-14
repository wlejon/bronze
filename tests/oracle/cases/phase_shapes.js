// A site that met one shape while its objects were being built and another
// ever after is read as a phase (docs/type-feedback.md, "Phases"): its claim
// is the later shape's alone, and an unfinished object that comes back to it
// leaves the speculated code. Each case below runs both phases, then an
// unfinished object again, and a site behind a recursion that meets a child
// before its parent.
class Point {
  constructor(owner, x, y) { this._x = x; this._y = y; this._owner = owner; }
  get x() { return this._x; }
  set x(v) { if (this._x !== v) { this._x = v; this._owner.touch(this); } }
}
class Node {
  constructor(name) {
    this.name = name; this.tick = 0; this.dirty = false; this.parent = null; this.children = [];
    this.pos = new Point(this, 0, 0); this.sum = 0; this.kind = "node";
  }
  get x() { return this.pos.x; }
  set x(v) { this.pos.x = v; }
  touch(p) {
    if (p === this.pos) this.tick++;
    if (this.dirty) return;
    this.dirty = true;
  }
}
class Leaf extends Node { constructor(name) { super(name); this.kind = "leaf"; this.weight = 1; } }

function build(name, i) {
  const n = new Leaf(name + i);
  n.x = i;          // the setter and touch() run on an object without vx, vy
  n.vx = i % 7;     // the shape every later frame meets
  n.vy = i % 3;
  return n;
}
function frame(list) {
  let t = 0;
  for (let i = 0; i < list.length; i++) {
    const n = list[i];
    n.dirty = false;
    n.x += n.vx;
    t += n.tick + n.vy;
  }
  return t;
}
// A site behind a recursion: the first receiver is the deepest child, then
// its parent once, then children for good.
function visit(n, depth) {
  for (let i = 0; i < n.children.length; i++) visit(n.children[i], depth + 1);
  n.sum = depth + (n.weight === undefined ? 100 : n.weight) + n.children.length;
  return n.sum;
}

const list = [];
for (let i = 0; i < 300; i++) list.push(build("n", i));
let total = 0;
for (let f = 0; f < 200; f++) total += frame(list);
console.log("frames", total);

// An unfinished object reaches the phased sites again, in a frame.
const late = new Leaf("late");
late.x = 5;
list.push(late);
let lateTotal = 0;
for (let f = 0; f < 5; f++) lateTotal += frame(list);
console.log("late", lateTotal, late.x, late.vx, late.tick, Number.isNaN(late.x));
late.vx = 2; late.vy = 1;
for (let f = 0; f < 50; f++) lateTotal += frame(list);
console.log("late finished", lateTotal, late.x, late.tick);

const root = new Node("root");
for (let i = 0; i < 40; i++) { const c = build("c", i); c.parent = root; root.children.push(c); }
let v = visit(root, 0);
for (let r = 0; r < 300; r++) for (let i = 0; i < root.children.length; i++) v += visit(root.children[i], 1);
v += visit(root, 0);
console.log("visit", v, root.sum, root.children[3].sum);

// Shapes that alternate for good are no phase.
const a = { k: 1, p: 2 }, b = { k: 3, p: 4, q: 5 };
function alt(o) { return o.k + o.p; }
let s = 0;
for (let i = 0; i < 2000; i++) s += alt(i & 1 ? a : b);
console.log("alternate", s);

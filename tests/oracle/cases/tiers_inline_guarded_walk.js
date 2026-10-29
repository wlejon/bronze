// The calls outside every loop of a method that a loop enters for real (here
// a walk down a tree: `update` calls `update` on each child) are compiled in
// place too (lower/inline_guarded.cpp, markEnteredFromLoops). Each stays
// behind its site's method cache, so a callee redefined after warm-up, a
// subclass's override, a throw, and the frames a stack trace names are as if
// every call had been made.

function frames(stack) {
  const out = [];
  for (const line of stack.split('\n').slice(1)) {
    const m = line.match(/at (?:(\S+) \()?.*?([^\/\\]+\.js):(\d+):(\d+)/);
    if (!m) continue;
    const name = m[1] || '<top>';
    if (name.startsWith('main')) break;
    out.push(name + '@' + m[3]);
  }
  return out.join(' < ');
}

class Mat {
  constructor() { this.e = [1, 0, 0, 1]; this.v = 0; }
  compose(a, b) {
    if (a > 900) throw new RangeError('compose ' + a);
    const e = this.e;
    e[0] = a; e[1] = b; e[2] = -b; e[3] = a;
    this.v++;
    return this;
  }
}

class Node {
  constructor(name, kids) {
    this.name = name;
    this.children = kids;
    this.parent = null;
    for (const k of kids) k.parent = this;
    this.m = new Mat();
    this.t = 0;
    this.d = 0;
    this.extra = 0;
  }
  local() { this.m.compose(this.t, this.t * 2); }
  depth() { return this.parent === null ? 0 : this.parent.depth() + 1; }
  count() { return arguments.length * 10 + (new.target === undefined ? 1 : 2); }
  update(t) {
    this.t = t;
    this.local();
    this.d = this.depth();
    this.extra = this.count(t, t);
    const kids = this.children;
    for (let i = 0; i < kids.length; i++) kids[i].update(t + 1);
  }
  sum() {
    let s = this.m.e[0] + this.m.e[1] + this.d + this.extra;
    const kids = this.children;
    for (let i = 0; i < kids.length; i++) s += kids[i].sum();
    return s;
  }
}

class Special extends Node {
  local() { this.m.compose(100, 0); }
}

function tree() {
  return new Node('root', [
    new Node('a', [new Node('a1', []), new Special('a2', [])]),
    new Node('b', [new Node('b1', [new Node('b11', [])])]),
  ]);
}

function main0() {
  const root = tree();
  let warm = 0;
  for (let k = 0; k < 500; k++) {
    root.update(k % 3);
    warm += root.sum();
  }
  console.log('warm', warm, root.m.v);

  root.update(10);
  console.log('update', root.sum());

  // An override replaced on the prototype after warm-up.
  Node.prototype.local = function () { this.m.compose(-1, -1); };
  root.update(10);
  console.log('redefined', root.sum());

  // The innermost callee replaced too.
  Mat.prototype.compose = function (a, b) { this.e[0] = a * 1000; this.e[1] = b; return this; };
  root.update(1);
  console.log('compose redefined', root.sum());

  // A throw from the innermost inlined body, through the walk.
  delete Node.prototype.local;
  Node.prototype.local = function () { this.m.compose(this.t, 0); };
  Mat.prototype.compose = function (a, b) { if (a > 900) throw new RangeError('compose ' + a); this.e[0] = a; this.e[1] = b; return this; };
  const fresh = tree();
  for (let k = 0; k < 300; k++) fresh.update(k % 3);
  try { fresh.update(899); } catch (e) { console.log('thrown', e.message); }
  console.log('partial', fresh.t, fresh.children[0].t, fresh.children[0].children[0].t, fresh.children[1].t);
}

function main1() {
  // The class's own methods, warmed and then thrown through: every frame of
  // the inlined chain is named at its own line.
  const root = tree();
  for (let k = 0; k < 300; k++) root.update(k % 3);
  try { root.update(899); } catch (e) { console.log('class thrown', e.message, frames(e.stack)); }
}

main1();
main0();

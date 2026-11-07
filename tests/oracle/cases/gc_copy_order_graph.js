// An object graph shaped like a scene graph — nodes with more properties than
// fit inline (an out-of-line slot block), small objects in fields, arrays of
// numbers in objects, children arrays naming more nodes, and a long chain —
// built with garbage between its parts and then copied by collections, which
// place what an object names right after it, depth first. Every value is read
// back by walking the graph and checked against totals kept outside it, in a
// Float64Array, as the graph was built.

class Vec {
  constructor(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  }
}

class Mat {
  constructor(seed) {
    this.elements = [seed, 1, 2, 3, 4, seed, 6, 7, 8, 9, seed, 11, 12, 13, 14, seed];
  }
}

class Node {
  constructor(id) {
    this.id = id;
    this.name = "n" + id;
    this.parent = null;
    this.children = [];
    this.position = new Vec(id, id + 1, id + 2);
    this.scale = new Vec(1, 2, 3);
    this.matrix = new Mat(id);
    this.matrixWorld = new Mat(id * 2);
    this.p8 = 8;
    this.p9 = 9;
    this.p10 = 10;
    this.p11 = 11;
    this.p12 = 12;
    this.p13 = 13;
    this.p14 = 14;
    this.p15 = 15;
    this.visible = true;
    this.layers = { mask: id & 7 };
    this.userData = { tag: "u" + (id % 5), list: [id, id + 1] };
    this.order = id % 3;
  }
}

const totals = new Float64Array(4);
let nextId = 0;
let garbage = [];

function addNode(parent) {
  garbage.push({ junk: nextId, more: [nextId, nextId] });
  if (garbage.length > 50) garbage = [];
  const n = new Node(nextId++);
  totals[0] += 1;
  totals[1] += n.position.x + n.position.y + n.position.z;
  totals[2] += n.matrix.elements[0] + n.matrixWorld.elements[15] + n.layers.mask + n.userData.list[1];
  if (parent !== null) {
    n.parent = parent;
    parent.children.push(n);
  }
  return n;
}

function grow(parent, depth) {
  if (depth === 0) return;
  for (let i = 0; i < 2; i++) grow(addNode(parent), depth - 1);
}

const roots = [];
for (let t = 0; t < 20; t++) {
  const r = addNode(null);
  grow(r, 5);
  roots.push(r);
}

// A chain far deeper than any copy goes in one step.
let chain = null;
for (let i = 0; i < 1500; i++) chain = { next: chain, v: i, pad: [i] };

function walk(n, acc) {
  acc[0] += 1;
  acc[1] += n.position.x + n.position.y + n.position.z;
  acc[2] += n.matrix.elements[0] + n.matrixWorld.elements[15] + n.layers.mask + n.userData.list[1];
  if (n.name !== "n" + n.id || n.p15 !== 15 || n.scale.z !== 3 || !n.visible) acc[3] += 1;
  for (const c of n.children) {
    if (c.parent !== n) acc[3] += 1;
    walk(c, acc);
  }
}

function check(round) {
  const acc = [0, 0, 0, 0];
  for (const r of roots) walk(r, acc);
  let len = 0;
  let sum = 0;
  for (let c = chain; c !== null; c = c.next) {
    len++;
    sum += c.v + c.pad[0];
  }
  console.log(
    round,
    acc[0] === totals[0],
    acc[1] === totals[1],
    acc[2] === totals[2],
    acc[3],
    acc[0],
    len,
    sum,
  );
}

check(0);
// Churn: enough short-lived allocation for collections to copy and promote
// the graph, with the graph mutated between them.
for (let round = 1; round <= 4; round++) {
  for (let i = 0; i < 3000; i++) garbage.push({ i, a: [i, i, i] });
  garbage = [];
  for (const r of roots) {
    const m = r.matrixWorld.elements;
    for (let k = 0; k < 16; k++) m[k] = m[k] + 0;
    r.position = new Vec(r.position.x, r.position.y, r.position.z);
  }
  check(round);
}

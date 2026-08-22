// The overflow-store bound (llvm_prop_set.cpp): the inline write guard
// compares a word index that INCLUDES the block's header word, so the correct
// bound is the block's total words — the old `capacity` bound was one short
// and permanently refused the LAST overflow slot (three.js's
// `renderItem.group`, 1.8M helper calls a run). This pins writes into every
// slot of objects wide enough to reach each block boundary, hit repeatedly so
// the inline path — not just the first filling write — answers.

// An 8-property object: slots 0..7, the renderItem shape. Slot 7 is the last
// word of a 4-slot overflow block — the exact slot the old bound refused.
function makeItem(i) {
  return {
    id: i, object: null, geometry: null, material: null,
    groupOrder: 0, renderOrder: 0, z: 0, group: null,
  };
}
const items = [];
for (let i = 0; i < 6; i++) items.push(makeItem(i));
let sum = 0;
for (let pass = 0; pass < 30000; pass++) {
  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    it.group = pass & 7;          // the last-slot depth-0 store
    it.z = pass * 0.5;            // a mid-overflow store beside it
    sum += it.group + it.z;
  }
}
console.log("sum:" + sum);
console.log(JSON.stringify(items[3]));

// Widths 1..14: every add walks the transition arm across each block
// boundary, and the read-back proves each store landed where the shape says.
for (let width = 1; width <= 14; width++) {
  const o = {};
  for (let k = 0; k < width; k++) o["k" + k] = k * 10;
  // overwrite every slot through the depth-0 hit path, twice
  for (let round = 0; round < 2; round++) {
    for (let k = 0; k < width; k++) o["k" + k] = k * 10 + round;
  }
  const parts = [];
  for (const k in o) parts.push(k + "=" + o[k]);
  console.log(width + ": " + parts.join(","));
}

// The class-instance spelling of the same shape: a 14-field constructor whose
// last fields sit at the end of a grown block, written per "frame".
class GraphNode {
  constructor(id) {
    this.id = id;
    this.name = "n" + id;
    this.depth = 0;
    this.x = 0;
    this.y = 0;
    this.z = 0;
    this.worldX = 0;
    this.worldY = 0;
    this.worldZ = 0;
    this.flag = false;
    this.weight = 1.0;
    this.tags = [];
    this.children = [];
    this.parent = null;
  }
}
const nodes = [];
for (let i = 0; i < 40; i++) nodes.push(new GraphNode(i));
for (let pass = 0; pass < 5000; pass++) {
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    n.worldX = n.x + pass;
    n.worldY = n.y + pass * 2;
    n.worldZ = n.z + pass * 3;
    n.flag = !n.flag;
    n.parent = pass & 1 ? null : nodes[0];
  }
}
console.log(nodes[7].worldX, nodes[7].worldY, nodes[7].worldZ, nodes[7].flag,
            nodes[7].parent === null);

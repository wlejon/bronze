// Instances whose constructor defines non-writable own data properties
// (Object.defineProperties with a value, as three.js's Object3D does), read
// and written on a hot path. The reads take the speculated slot load; a store
// to a writable key is a slot store; a store to a read-only key must still be
// refused (silently here: sloppy code) and leave the value as it was.
function Node(i) {
  this.id = i;
  Object.defineProperties(this, {
    pos: { configurable: true, enumerable: true, value: { x: i, y: 2 * i } },
    scale: { configurable: true, enumerable: true, value: 3 },
  });
  this.w = i % 5;
}
function readAll(n) {
  return n.pos.x + n.pos.y + n.scale + n.w + n.id;
}
function bump(n, v) {
  n.w = n.w + v;
  n.scale = v;
  return n.scale;
}
function main() {
  const nodes = [];
  for (let i = 0; i < 64; i++) nodes.push(new Node(i));
  let s = 0;
  for (let r = 0; r < 3000; r++) {
    for (let i = 0; i < nodes.length; i++) {
      s += readAll(nodes[i]);
      s += bump(nodes[i], 1);
    }
  }
  console.log(s, nodes[7].w, nodes[7].scale, Object.getOwnPropertyDescriptor(nodes[7], "scale").writable);
}
main();

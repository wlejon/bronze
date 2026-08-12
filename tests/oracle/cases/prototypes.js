// Constructor + method on the prototype: the shape of `p` records
// Point.prototype, so `p.dist2` misses on the own properties and finds the
// method one link up.
function Point(x, y) {
  this.x = x;
  this.y = y;
}
Point.prototype.dist2 = function () {
  return this.x * this.x + this.y * this.y;
};
Point.prototype.kind = "point";

const p = new Point(3, 4);
console.log(p.x);
console.log(p.y);
console.log(p.dist2());
console.log(p.kind);

// Two instances share the prototype's method but not its own properties.
const q = new Point(6, 8);
console.log(q.dist2());
console.log(p.x);

// An own property shadows the prototype's, and assigning it does NOT touch
// the prototype: the write creates an own property on the receiver.
q.kind = "shadowed";
console.log(q.kind);
console.log(p.kind);

// A property nowhere on the chain is undefined, not an error.
console.log(p.missing);

// The prototype object is reachable as a value, and it is the same object
// every time the name is mentioned.
const proto = Point.prototype;
proto.tag = 42;
console.log(p.tag);

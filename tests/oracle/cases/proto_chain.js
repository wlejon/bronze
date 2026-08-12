// A two-link chain: `d.kind` is not on the instance and not on
// Derived.prototype either — it is one more link up, on Base.prototype. The IC
// caches the depth it walked, so the second lookup is a shape compare plus two
// pointer chases.
function Base() {}
Base.prototype.kind = "base";
Base.prototype.describe = function () {
  return "a " + this.kind;
};

// `own` occupies slot 0 of an instance, which is also the slot `kind`
// occupies on Base.prototype. A cache that remembers the slot but forgets
// the depth reads `own` and looks plausible, so the two values are
// deliberately different lengths as well as different strings.
function Derived() {
  this.own = "mine-and-longer";
}
Derived.prototype = new Base();
Derived.prototype.extra = "derived";

const d = new Derived();
console.log(d.own);
console.log(d.extra);
console.log(d.kind);
console.log(d.describe());

// The same site, hit repeatedly: the first pass fills the cache and the
// rest hit it. A wrong depth or a stale shape shows up as a wrong sum.
let total = 0;
let i = 0;
while (i < 1000) {
  total = total + d.kind.length;
  i = i + 1;
}
console.log(total);

// The same site seeing two different receiver shapes: the cache is
// monomorphic, so this must miss and re-walk rather than reuse a depth
// that belongs to the other shape.
function pick(o) {
  return o.kind;
}
const b = new Base();
console.log(pick(d));
console.log(pick(b));
console.log(pick(d));

// A method found on the prototype is called with the receiver as `this`,
// so two instances of the same constructor get different answers from one
// shared function object.
const d2 = new Derived();
d2.kind = "override";
console.log(d2.describe());
console.log(d.describe());

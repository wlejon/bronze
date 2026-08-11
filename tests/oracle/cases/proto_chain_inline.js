// proto_chain.js with the receivers arranged so every hot site is PROVEN
// monomorphic and therefore takes the inlined cache path of docs/0010
// decision 7. The point is the same one docs/0008 decision 2 makes and it
// bites harder here, because the check that must not forget the depth is
// now open-coded in the object file instead of living in one helper.
//
// Three links, and every property below is at slot 0 of whatever object
// holds it, at three different depths:
//
//   leaf ---0--> own            (slot 0 of the instance)
//   leaf ---2--> middleOnly     (slot 0 of Middle.prototype)
//   leaf ---3--> name           (slot 0 of Root.prototype)
//
// So an inline path that compares the cached slot and drops the cached
// depth answers "leaf-slot-zero" to all three and looks entirely
// plausible. The three strings are different lengths as well as different
// text, so a partial read cannot pass either.
function Root() {}
Root.prototype.name = "root-name";
Root.prototype.describe = function () {
  return "I am " + this.name;
};

function Middle() {}
Middle.prototype = new Root();
Middle.prototype.middleOnly = "middle";

function Leaf() {
  this.own = "leaf-slot-zero";
}
Leaf.prototype = new Middle();

const leaf = new Leaf();
console.log(leaf.own);
console.log(leaf.middleOnly);
console.log(leaf.name);
console.log(leaf.describe());

// The same sites hit a thousand times, so the cached entry rather than the
// first walk is what answers. A wrong depth shows up immediately in the
// length sum.
let total = 0;
let i = 0;
while (i < 1000) {
  total = total + leaf.name.length + leaf.own.length;
  i = i + 1;
}
console.log(total);

// A second instance of the same class, given an OWN `name` that shadows
// the one three links up. Its shape is no longer the class's shape, so the
// site's guard has to reject the cached entry rather than trust the proof.
const leaf2 = new Leaf();
leaf2.name = "leaf2-own-name";
console.log(leaf2.own);
console.log(leaf2.name);
console.log(leaf.name);
console.log(leaf2.describe());
console.log(leaf.describe());

// A miss all the way off the end of a three-link chain.
console.log(leaf.missing);

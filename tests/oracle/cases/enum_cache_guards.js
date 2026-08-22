// The shape-keyed enumeration cache (rt_enumerate.cpp): every guard that can
// invalidate a cached key list, pinned. The cache answers `for-in`'s key
// snapshot from (holder shape, prototype epoch) when the chain is provable, so
// each section below moves exactly one of those and checks the NEXT loop sees
// the world as it is now.
//
// Deliberately absent: deleting a not-yet-visited key DURING a loop. Bronze
// enumerates a snapshot taken at loop entry (rt_enumerate.cpp's header states
// it), so it visits such a key where a spec engine must not — a pinned-vs-node
// case cannot hold both. The seam A/B (BRONZE_NO_ENUM_CACHE=1) covers that
// corner instead: cache on and off build the same snapshot by construction.

function keysOf(o) {
  const seen = [];
  for (const k in o) seen.push(k);
  return seen.join(",");
}

// ---- same shape, repeated and shared: the cache's hit case ----------------
class Node {
  constructor(id) {
    this.id = id;
    this.tag = "n";
    this.weight = id * 2;
  }
}
const a = new Node(1);
const b = new Node(2);
console.log(keysOf(a));
console.log(keysOf(b));
console.log(keysOf(a)); // the repeat that must come from the cache

// ---- an own add transitions the shape: the next loop sees the new key -----
a.extra = true;
console.log(keysOf(a));
console.log(keysOf(b)); // b kept the old shape and the old list

// ---- an own delete demotes to dictionary mode: refused, still correct -----
delete a.tag;
console.log(keysOf(a));
a.tag = "readded";
console.log(keysOf(a)); // re-created keys go to the END (10.1.11)

// ---- a prototype ADD between loops (the epoch guard) ----------------------
class Base {
  constructor() {
    this.own = 1;
  }
}
class Derived extends Base {
  constructor() {
    super();
    this.mine = 2;
  }
}
const d1 = new Derived();
const d2 = new Derived();
console.log(keysOf(d1));
Base.prototype.addedOnProto = "later";
console.log(keysOf(d1)); // the inherited enumerable key appears
console.log(keysOf(d2)); // and for every object of the shape

// ---- shadowing: an own enumerable key hides the inherited one -------------
d1.addedOnProto = "shadow";
console.log(keysOf(d1)); // visited ONCE, at the nearest level
console.log(d1.addedOnProto);
console.log(d2.addedOnProto);

// ---- a prototype DELETE between loops (the chain re-walk guard) -----------
delete Base.prototype.addedOnProto;
console.log(keysOf(d2)); // the inherited key is gone again
console.log(keysOf(d1)); // the shadow was an OWN key and stays

// ---- re-entrant: a loop inside a loop over the same object ----------------
const outerSeen = [];
for (const k1 in d2) {
  const innerSeen = [];
  for (const k2 in d2) innerSeen.push(k2);
  outerSeen.push(k1 + "[" + innerSeen.join("+") + "]");
}
console.log(outerSeen.join(" "));

// ---- an add DURING the loop: the snapshot does not grow -------------------
const grow = { p: 1, q: 2 };
const growSeen = [];
for (const k in grow) {
  growSeen.push(k);
  grow["late_" + k] = true;
}
console.log(growSeen.join(","));
console.log(keysOf(grow)); // the next loop sees everything

// ---- an array with named own keys: indices first, then the names ----------
const arr = [10, 20, 30];
arr.label = "L";
console.log(keysOf(arr));
delete arr[1];
console.log(keysOf(arr)); // the hole leaves the key list

// ---- an empty object and an empty chain -----------------------------------
console.log("empty:[" + keysOf({}) + "]");

// Enumeration order once a delete has taken the shape chain away from it
// (docs/0019 decision 1, completing docs/0009 decision 1).
//
// docs/0009 recovers own-key order from the shape transition chain, which is
// already a list of the properties in insertion order and costs nothing to
// walk. A delete cannot remove a node from that chain — shapes are shared —
// so the object moves to a table it owns, and the table has to reproduce
// every rule the chain gave for free:
//
// 1. THE KEYS THAT REMAIN KEEP THEIR RELATIVE ORDER. A delete from the
//    middle closes up; it does not reshuffle.
// 2. A RE-ADDED KEY GOES TO THE END. ECMA-262 10.1.11's ordering is by
//    creation, and a delete followed by a create is a new creation. This is
//    the one rule only a delete can make visible, and it is the reason
//    `delete o.b; o.b = 9` is not a no-op even though the value is restored.
// 3. INTEGER-LIKE KEYS STILL COME FIRST, ASCENDING, whatever order they were
//    created or deleted in — that half of 10.1.11 is about the KEY, not about
//    when it arrived, so it must survive the move to a table intact.
// 4. THE SAME ORDER REACHES EVERY CONSUMER. `Object.keys`, `for-in`, object
//    spread and `console.log`'s inspect format all ask one question
//    (`rtOwnKeysOrdered`), so a dictionary that answered only the first would
//    be caught here by the other three.
// 5. A NON-ENUMERABLE PROPERTY STAYS NON-ENUMERABLE ACROSS THE MOVE. A class
//    method is defined with `enumerable: false` (15.7.14, docs/0018 decision
//    2), and the attribute is carried into the table with the key.

// ---- the delete/re-add distinction ----------------------------------------
const o = { a: 1, b: 2, c: 3, d: 4 };
console.log(Object.keys(o).join(","));
delete o.b;
console.log(Object.keys(o).join(","));
delete o.d;
console.log(Object.keys(o).join(","));
o.b = 20;
console.log(Object.keys(o).join(","));
o.d = 40;
console.log(Object.keys(o).join(","));
// Re-assigning an EXISTING key does not move it: only creation orders.
o.a = 100;
console.log(Object.keys(o).join(","));
console.log(o);

// ---- integer-like keys sort ahead of the string ones, still ---------------
const m = { z: "z", "10": "ten", y: "y", "2": "two", "1": "one" };
delete m.y;
delete m["2"];
m.x = "x";
m["7"] = "seven";
console.log(Object.keys(m).join(","));

const seen = [];
for (const k in m) {
  seen.push(k);
}
console.log(seen.join("|"));

const copy = { ...m };
console.log(Object.keys(copy).join(","));
console.log(copy);

// ---- the enumerable attribute survives the move ---------------------------
class Point {
  constructor(x, y) {
    this.x = x;
    this.y = y;
    this.tag = "p";
  }
  norm() {
    return this.x + this.y;
  }
}
const p = new Point(1, 2);
delete p.x;
p.x = 9;
console.log(Object.keys(p).join(","));
const keys = [];
for (const k in p) {
  keys.push(k);
}
console.log(keys.join("|"));
console.log(p.norm());

// ---- an array's holes leave the key list, and `length` does not move ------
const arr = [1, 2, 3, 4];
delete arr[1];
delete arr[3];
console.log(arr.length);
console.log(Object.keys(arr).join(","));
const idx = [];
for (const i in arr) {
  idx.push(i);
}
console.log(idx.join("|"));
console.log(arr);
console.log(arr[1]);
console.log(1 in arr);
console.log(2 in arr);

// A Map whose keys are OBJECTS, run hard enough that the collector moves them
// under it. This is the case the Map index's epoch exists for: a Map key is a
// value, not a property name, so an object key can only be hashed by its
// ADDRESS — and a semispace collector changes every address it copies. The
// table records the collection count it was indexed at and rebuilds when that
// count has moved on.
//
// It matters that this case ALLOCATES between lookups (the `junk` object in
// the probe loop, the pair arrays the Map iterator hands out), because under
// oracle-gc-stress every one of those allocations is a collection: a stale
// bucket would answer "not found" for a key the map still holds, which is a
// silent wrong answer rather than a crash. The map also has to keep its keys
// and values ALIVE — nothing else references the entries once `keys` has been
// walked past — so a payload the collector failed to scan shows up here as a
// wrong `id` rather than as a missing entry.
//
// From ECMA-262 24.1 (Map) and 24.2 (Set):
//
// 1. Object keys are identity keys: a structurally identical object is a
//    different key, and `has` on one answers false.
// 2. Deleting and re-adding moves an entry to the END of the iteration
//    order, which is what makes the first three pairs 1, 2, 4 rather than
//    0, 1, 2 after the multiples of three come back.
// 3. `forEach` takes (value, key), the opposite order from `entries`.
// 4. `keys()`, `values()` and `entries()` are iterators — objects with a
//    `next`, spreadable, and steppable by hand — not arrays.
// 5. `clear` empties the table without replacing it.

function node(i) {
  return { id: i, pad: "n" + i };
}

const byNode = new Map();
const keys = [];
for (let i = 0; i < 60; i = i + 1) {
  const k = node(i);
  keys.push(k);
  byNode.set(k, i * 2);
}
console.log(byNode.size);

let found = 0;
let missing = 0;
for (let i = 0; i < 60; i = i + 1) {
  const junk = { a: i, b: "" + i };
  if (byNode.get(keys[i]) === i * 2) found = found + 1;
  else missing = missing + 1;
  if (byNode.has(junk)) missing = missing + 1;
}
console.log(found, missing);

let removed = 0;
for (let i = 0; i < 60; i = i + 3) {
  if (byNode.delete(keys[i])) removed = removed + 1;
}
console.log(removed, byNode.size);

let sum = 0;
for (const pair of byNode) {
  sum = sum + pair[1];
}
console.log(sum);

for (let i = 0; i < 60; i = i + 3) {
  byNode.set(keys[i], i * 2);
}
console.log(byNode.size);

let order = "";
let n = 0;
for (const [k, v] of byNode) {
  if (n < 3) order = order + k.id + ":" + v + ";";
  n = n + 1;
}
console.log(n, order);

let last = "";
byNode.forEach(function (v, k) {
  last = k.id + "=" + v;
});
console.log(last);

const strings = new Set();
for (let i = 0; i < 40; i = i + 1) {
  strings.add("s" + (i % 10));
}
console.log(strings.size, [...strings].join(","));

const it = strings.values();
console.log(it.next().value, it.next().value, it.next().done);

const kv = new Map();
kv.set("a", 1);
kv.set("b", 2);
console.log([...kv.keys()].join(","), [...kv.values()].join(","));
console.log([...kv.entries()].map(function (e) { return e[0] + e[1]; }).join(","));
kv.clear();
console.log(kv.size, [...kv].length);

// WeakMap (ECMA-262 24.3): the four methods, the CanBeHeldWeakly split, the
// iterable constructor argument, and the two absences that make the type what
// it is — no size, no iteration.
//
// Derived from ECMA-262, not from bronze's output:
//
// 1. 24.3.3.5 `set` returns the map itself, so `.set` chains and `=== wm`
//    holds.
// 2. Keys are compared by IDENTITY: a second `{}` with the same shape is a
//    different key (24.3.3.3 finds nothing for it).
// 3. 24.3.3.2 `delete` answers whether something was removed: true once,
//    false the second time.
// 4. The reads are quiet about an invalid key — 24.3.3.3/24.3.3.4/24.3.3.2
//    step 4 return undefined/false BEFORE touching the table — while the
//    WRITE throws: 24.3.3.5 step 4 is a TypeError for a key that cannot be
//    held weakly.
// 5. 24.3.1.1 walks an iterable of [key, value] entries through the same
//    adder.
// 6. 24.3.3.6 gives `WeakMap.prototype` a @@toStringTag of "WeakMap", which
//    `Object.prototype.toString` (20.1.3.6 step 15) reads.
// 7. 24.3.3 defines NO `size` and NO `[Symbol.iterator]`: the first reads as
//    a property that does not exist, the second as undefined — a WeakMap is
//    not iterable, and that is half of what "weak" means observably.
const wm = new WeakMap();
const k1 = {};
const k2 = {};
console.log(wm.set(k1, 1) === wm);
console.log(wm.has(k1), wm.has(k2));
console.log(wm.get(k1), wm.get(k2));
wm.set(k2, { deep: 2 });
console.log(wm.get(k2).deep);
console.log(wm.delete(k1), wm.delete(k1));
console.log(wm.has(k1));
console.log(wm.has(1), wm.get('x'), wm.delete(true));
try {
  wm.set(1, 2);
} catch (e) {
  console.log(e instanceof TypeError);
}
const k3 = {};
const wm2 = new WeakMap([[k3, 'v']]);
console.log(wm2.get(k3));
console.log(typeof wm);
console.log(Object.prototype.toString.call(wm));
console.log(wm[Symbol.iterator]);
console.log('size' in wm);
console.log('set' in wm, 'get' in wm);

// Array.prototype.sort's merge engine (builtin_array_sort.cpp): the list is
// merged inside one rooted block, so these pin what that must keep — stability
// over large inputs, Values that survive collections the comparator causes,
// the comparator's answer through ToNumber, and every receiver the engine
// sees through the same snapshot and write-back.

// --- 1. stability on a large input, three.js painter-sort shaped -------------
const mats = [];
for (let i = 0; i < 7; i++) mats.push({ id: i });
let seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }
const items = [];
for (let i = 0; i < 3000; i++) {
  items.push({ id: i, groupOrder: i % 3 === 0 ? 1 : 0, renderOrder: 0, material: mats[i % 7], z: Math.floor(rnd() * 10) });
}
function painter(a, b) {
  if (a.groupOrder !== b.groupOrder) return a.groupOrder - b.groupOrder;
  if (a.renderOrder !== b.renderOrder) return a.renderOrder - b.renderOrder;
  if (a.material.id !== b.material.id) return a.material.id - b.material.id;
  return a.z - b.z;
}
items.sort(painter);
let stable = true;
for (let i = 1; i < items.length; i++) {
  const c = painter(items[i - 1], items[i]);
  if (c > 0 || (c === 0 && items[i - 1].id > items[i].id)) stable = false;
}
console.log(items.length, stable, items[0].id, items[1].id, items[2999].id);

// --- 2. a comparator that allocates heavily (collections mid-sort) ----------
const strs = [];
for (let i = 0; i < 500; i++) strs.push({ k: 'key' + ((i * 7919) % 500), n: i });
let garbage = [];
strs.sort((a, b) => {
  garbage.push({ a: a.k + '/' + b.k, arr: [a.n, b.n] });
  if (garbage.length > 64) garbage = [];
  return a.k < b.k ? -1 : a.k > b.k ? 1 : 0;
});
let ordered = true;
for (let i = 1; i < strs.length; i++) if (strs[i - 1].k > strs[i].k) ordered = false;
console.log(ordered, strs[0].k, strs[0].n, strs[499].k, strs[499].n);

// --- 3. answers that are not numbers ----------------------------------------
console.log([3, 1, 2].sort((a, b) => String(a - b)).join(','));
console.log([3, 1, 2].sort((a, b) => ({ valueOf() { return a - b; } })).join(','));
console.log([3, 1, 2].sort(() => NaN).join(','));
console.log([3, 1, 2].sort((a, b) => (a < b ? -Infinity : Infinity)).join(','));
console.log([3, 1, 2].sort((a, b) => (a > b ? 1n : -1n) && (a - b)).join(','));
let calls = 0;
try {
  [3, 1, 2].sort(() => { calls++; return Symbol('x'); });
} catch (e) {
  console.log(e instanceof TypeError, calls);
}

// --- 4. comparators of every callable kind ------------------------------------
const byNum = function (a, b) { return this.dir * (a - b); }.bind({ dir: -1 });
console.log([5, 1, 4, 2].sort(byNum).join(','));
class Cmp { static by(a, b) { return a - b; } }
console.log([5, 1, 4, 2].sort(Cmp.by).join(','));
console.log([5, 1, 4, 2].sort(new Proxy((a, b) => b - a, {})).join(','));
let threwClass = false;
try { [2, 1].sort(class {}); } catch (e) { threwClass = e instanceof TypeError; }
console.log(threwClass);
console.log(['b', 'a', 'c'].sort(String.prototype.localeCompare.call.bind(String.prototype.localeCompare)).join(','));

// --- 5. undefined, holes and a thrown error ------------------------------------
const mixed = [3, undefined, 1, , 2, undefined, , 0];
mixed.sort((a, b) => a - b);
console.log(mixed.length, JSON.stringify(mixed), 6 in mixed, 7 in mixed);
const keep = [4, 3, 2, 1];
let n = 0;
try { keep.sort((a, b) => { if (++n === 3) throw new Error('stop'); return a - b; }); } catch (e) { console.log(e.message, keep.join(',')); }

// --- 6. mutation of the receiver mid-sort -------------------------------------
const mut = [5, 4, 3, 2, 1];
mut.sort((a, b) => { mut.length = 2; return a - b; });
console.log(mut.length, mut.join(','));
const grow = [3, 2, 1];
grow.sort((a, b) => { grow.push(9); return a - b; });
console.log(grow.slice(0, 3).join(','), grow.length > 3);

// --- 7. array-likes, frozen and sealed receivers ---------------------------------
const like = { length: 4, 0: 'd', 1: 'b', 3: 'a' };
Array.prototype.sort.call(like);
console.log(JSON.stringify(like));
const frozen = Object.freeze([2, 1]);
try { frozen.sort(); } catch (e) { console.log(e instanceof TypeError, frozen.join(',')); }
const sortedFrozen = Object.freeze([1, 2]);
try { sortedFrozen.sort(); } catch (e) { console.log('sorted frozen', e instanceof TypeError); }
console.log([].sort().length, [1].sort((a, b) => { throw new Error('never'); }).join(','));

// --- 8. the default comparator over mixed values --------------------------------
console.log([10, 9, 1, 'a', true, null, undefined, 2n, -0, 0].sort().map(String).join(','));

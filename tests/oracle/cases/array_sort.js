// Array.prototype.sort (ECMA-262 23.1.3.30): the default comparator, a user
// comparator, stability, the undefined/hole ordering, a throwing comparator,
// and a comparator that mutates the array mid-sort.
//
// Derived from ECMA-262:
//
// 1. The default comparator is CompareArrayElements' ToString + code-unit
//    order (23.1.3.30.2 steps 5-9), so [10, 9, 1, 20] sorts as the STRINGS
//    "1" < "10" < "20" < "9".
// 2. Sort is required stable (23.1.3.30 sorts a List and 23.1.3.30.1 demands
//    a consistent comparator's order be preserved for equal elements): the
//    floor-equal pairs keep their input order, observed through the letters.
// 3. Undefined sorts to the END without the comparator ever seeing one
//    (23.1.3.30.2 steps 1-3), and `join` renders it as the empty string
//    (23.1.3.15 step 4) — hence the trailing commas.
// 4. A HOLE is skipped by SortIndexedProperties (skip-holes) and reappears
//    AFTER the undefineds as a delete of the tail index (23.1.3.30 step 9):
//    the sorted array has the same length, the elements first, the hole last
//    and `2 in holed` false.
// 5. A comparator that throws aborts the sort with the array UNTOUCHED: the
//    elements were read into a list up front, and the write-back only runs
//    for a sort that finished.
// 6. A comparator that MUTATES the array cannot corrupt the sort for the same
//    reason: the list was read before the first comparison, so the first
//    three slots hold the sorted snapshot; the pushed elements land past
//    them, and at least one comparison must happen to sort three unsorted
//    elements, so the length grew.
const nums = [10, 9, 1, 20];
console.log(nums.sort().join(','));
console.log(nums.sort((x, y) => x - y).join(','));
console.log(nums.sort((x, y) => x - y) === nums);
const items = [[2, 'a'], [1, 'b'], [2, 'c'], [1, 'd']];
items.sort((x, y) => x[0] - y[0]);
console.log(items.map((p) => p[0] + p[1]).join(','));
const withUndef = [undefined, 3, 1, undefined, 2];
withUndef.sort();
console.log(withUndef.join(','));
console.log(withUndef.length, withUndef[2], withUndef[3]);
const holed = [3, 0, 1];
delete holed[1];
holed.sort();
console.log(holed.length, 0 in holed, 1 in holed, 2 in holed);
console.log(holed[0], holed[1]);
console.log(['b', 'a', 'c'].sort().join(''));
const t = [3, 1, 2];
try {
  t.sort(() => { throw new TypeError('nope'); });
} catch (e) {
  console.log(e instanceof TypeError);
}
console.log(t.join(','));
const m = [3, 1, 2];
m.sort(function (x, y) { m.push(99); return x - y; });
console.log(m.slice(0, 3).join(','));
console.log(m.length > 3);

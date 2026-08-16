// 20.1.2.13 Object.groupBy, which is GroupBy (7.3.35) with keyCoercion
// "property", poured into an object created with OrdinaryObjectCreate(null) —
// so the result has NO prototype, which the first line pins: a grouped result
// never inherits `toString` or `hasOwnProperty`, and `Object.getPrototypeOf`
// answers null. That is not a detail of the pouring; it is what keeps a group
// named "toString" from colliding with an inherited member.
//
// The group keys appear in FIRST-OCCURRENCE order (7.3.35 step 6 appends a new
// group at the end of `groups`), and each group's elements keep the iteration
// order of the source. The callback receives the element and the running index
// (7.3.35 step 6.c), which the last line pins.
//
// `Map.groupBy` (24.1.2.1) is the same algorithm with keyCoercion "collection";
// map_group_by.js pins the half that differs.
const g = Object.groupBy([1, 2, 3, 4, 5], (x) => (x % 2 === 0 ? "even" : "odd"));
console.log(Object.getPrototypeOf(g) === null);
console.log(Object.keys(g).join(","));
console.log(g.odd.join(","));
console.log(g.even.join(","));

const byIndex = Object.groupBy(["a", "b"], (x, i) => i);
console.log(byIndex[0].join(","), byIndex[1].join(","));

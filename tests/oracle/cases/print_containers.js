// console.log of a container, in the pinned inspect format: node's
// util.inspect, on one line. Strings are quoted INSIDE a container and raw at
// the top level; keys come out in the language's order; nesting past depth 2
// collapses to [Array] / [Object]; a cycle is marked rather than followed,
// which is the difference between this output and a hang.
//
// The last three lines pin a documented DIVERGENCE from node: nothing in the
// runtime carries a name, so a function prints `[Function]` and an instance
// prints its properties without its class name. When names land, this file's
// expectation gains them.
console.log([1, 2, 3]);
console.log([]);
console.log({});
console.log({ a: 1, b: "x" });
console.log("hi");
console.log(["hi", "it's"]);
console.log([true, false, null, undefined]);
console.log([-0, 0, 1 / 0, 0 / 0]);
console.log({ "a-b": 1, "2": "two", z: 3, a: 4 });
console.log([[1, [2, [3, [4]]]]]);
console.log([{ a: { b: { c: 1 } } }]);
console.log(["tab\there", "nl\nhere"]);
const cyclic = { name: "root", self: null };
cyclic.self = cyclic;
console.log(cyclic);
const arr = [1];
arr.push(arr);
console.log(arr);
console.log([function () {}]);
console.log(new Float32Array(3));
class Point {
  constructor(x, y) { this.x = x; this.y = y; }
  sum() { return this.x + this.y; }
}
console.log(new Point(1, 2));
console.log([new Point(1, 2), new Point(3, 4)]);

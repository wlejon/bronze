// Array.prototype.findLast / findLastIndex, self-hosted (src/selfhost): every
// index from the last, a hole read through the chain, the callback's
// arguments, receivers the merged copy does not take (array-likes, boxed
// arrays, subclasses, proxies), a length that changes under the walk, and
// the errors with their stacks.

function show(label, v) { console.log(label, typeof v === "string" ? JSON.stringify(v) : String(v)); }

function walk(a, t) { return [a.findLast((x) => x === t), a.findLastIndex((x) => x === t)].join(); }

const plain = [1, 2, 3, 2, 1];
for (let r = 0; r < 3; ++r) {
  for (const t of [1, 2, 3, 4]) show("plain " + t, walk(plain, t));
}
for (let r = 0; r < 2; ++r) show("empty", walk([], 1));

// Arguments: value, index, the object; thisArg.
const seen = [];
const ctx = { tag: "ctx" };
show("args", [10, 20, 30].findLast(function (v, i, o) { seen.push(v + "@" + i + "/" + o.length + ":" + this.tag); return false; }, ctx));
show("seen", seen.join(" "));
show("thisArg undefined strict", [1].findLastIndex(function () { "use strict"; return this === undefined; }));

// Holes: every index is visited, a hole reads as undefined, or through the chain.
const holey = [1, , 3];
show("holes", holey.findLastIndex((x) => x === undefined) + " " + holey.findLast((x, i) => i === 1));
Object.prototype[1] = "inherited";
show("holes chain", holey.findLast((x, i) => i === 1));
delete Object.prototype[1];

// Receivers the merged copy hands to the registered builtin.
const arrayLike = { length: 3, 0: "a", 1: "b", 2: "a" };
show("array-like", Array.prototype.findLastIndex.call(arrayLike, (x) => x === "a"));
show("string", Array.prototype.findLast.call("xyz", (c) => c < "z"));
const boxed = [5, 6, 7];
boxed.extra = true;
show("boxed", walk(boxed, 6));
class Sub extends Array {}
show("subclass", walk(Sub.from([4, 5, 4]), 4));
show("proxy", walk(new Proxy([8, 9, 8], {}), 8));
show("length getter", Array.prototype.findLastIndex.call({ get length() { return 2; }, 0: 1, 1: 1 }, (x) => x === 1));

// The length is read once; the walk sees elements as they are when reached.
const shrink = [1, 2, 3, 4];
show("shrink", shrink.findLastIndex((x, i, o) => { if (i === 3) o.length = 1; return x === undefined && i === 2; }));
const grow = [1, 2];
show("grow", grow.findLastIndex((x, i, o) => { o.push(9); return false; }) + " " + grow.length);

// Errors, caught, with the frame names Error.stack shows.
for (const bad of [undefined, null, 1, "f", {}]) {
  try { [1].findLast(bad); } catch (e) { show("not callable", e.constructor.name + ": " + e.message); }
}
try { Array.prototype.findLastIndex.call(null, () => true); } catch (e) { show("null this", e.constructor.name); }
function thrower() { return [1, 2].findLast(() => { throw new Error("cb"); }); }
try { thrower(); } catch (e) { show("callback throw", e.message + " " + /thrower/.test(String(e.stack))); }

show("name/length", Array.prototype.findLast.name + " " + Array.prototype.findLast.length + " " +
  Array.prototype.findLastIndex.name + " " + Array.prototype.findLastIndex.length);
show("toString", String(Array.prototype.findLast).includes("[native code]"));
show("no prototype", Array.prototype.findLast.prototype === undefined);
try { new Array.prototype.findLastIndex(() => true); } catch (e) { show("construct", e.constructor.name); }

// A hot loop, so the merged copy and its inlined callback run.
function hot() {
  const a = [];
  for (let i = 0; i < 500; ++i) a.push(i % 37);
  let s = 0;
  for (let r = 0; r < 300; ++r) s += a.findLastIndex((x) => x === r % 40) + (a.findLast((x) => x > r % 36) | 0);
  return s;
}
show("hot", hot());

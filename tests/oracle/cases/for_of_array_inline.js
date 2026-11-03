// for-of over arrays: the inline step (array record, cursor < length, not a
// hole) and every case it must hand to bronze_iter_step.

function collect(a) {
  let out = "";
  let sep = "";
  for (const v of a) { out += sep + v; sep = ","; }
  return out;
}

console.log("plain", collect([1, "two", { three: 3 }.three, null, undefined]));
console.log("empty", collect([]));
console.log("holes", collect([1, , 3, , ]));
// A hole reads through the chain. The index lives on an object between the
// array and Array.prototype, which bronze refuses to decorate with one.
{
  const p = Object.create(Array.prototype);
  p[1] = "P";
  const h = [1, , 3];
  Object.setPrototypeOf(h, p);
  console.log("holes proto", collect(h));
  const q = Object.create(Array.prototype);
  let reads = 0;
  Object.defineProperty(q, 2, { get() { reads++; return "G"; } });
  const g = [1, 2, , 4];
  Object.setPrototypeOf(g, q);
  console.log("holes getter", collect(g), reads);
}

// Mutation during the loop: growth is seen, shrinking stops early.
{
  const a = [1, 2, 3];
  const seen = [];
  for (const v of a) { seen.push(v); if (v < 6) a.push(v + 3); }
  console.log("grow", seen.join(","));
  const b = [1, 2, 3, 4, 5, 6];
  const seen2 = [];
  for (const v of b) { seen2.push(v); if (v === 2) b.length = 3; }
  console.log("shrink", seen2.join(","));
  const c = [1, 2, 3, 4];
  const seen3 = [];
  for (const v of c) { seen3.push(v); if (v === 1) c.shift(); }
  console.log("shift", seen3.join(","));
  const d = [1, 2, 3];
  const seen4 = [];
  for (const v of d) { seen4.push(v); if (v === 1) d[2] = "changed"; }
  console.log("write ahead", seen4.join(","));
}

// break / continue / return / throw, and resuming a fresh loop after each.
{
  const a = [1, 2, 3, 4, 5];
  let s = "";
  for (const v of a) { if (v === 2) continue; if (v === 4) break; s += v; }
  console.log("break/continue", s);
  function first(xs) { for (const x of xs) if (x > 2) return x; return -1; }
  console.log("return", first(a), first([]));
  try { for (const v of a) if (v === 3) throw new Error("at " + v); } catch (e) { console.log("throw", e.message); }
  let n = 0;
  for (const outer of [[1, 2], [3], []]) for (const inner of outer) n += inner;
  console.log("nested", n);
}

// An overridden `[Symbol.iterator]` must be honoured wherever it lives: on
// the array, on a subclass, on a prototype between the array and
// Array.prototype. (Array.prototype itself refuses the change in bronze, and
// bronze's array iterators carry `next` as an own property, so patching
// %ArrayIteratorPrototype%.next is not tested here.)
{
  const own = [1, 2];
  own[Symbol.iterator] = function* () { yield "own"; yield this.length; };
  console.log("own iterator", collect(own));
  const named = [1, 2];
  named.tag = "named";
  console.log("named property", collect(named));
  class Override extends Array { *[Symbol.iterator]() { yield "sub"; } }
  console.log("subclass override", collect(Override.from([3, 4])));
  class Plain extends Array {}
  console.log("subclass", collect(Plain.from([3, 4])));
  const p = Object.create(Array.prototype);
  p[Symbol.iterator] = function* () { yield "proto"; };
  const x = [5];
  Object.setPrototypeOf(x, p);
  console.log("proto override", collect(x));
  const values = [6, 7];
  values[Symbol.iterator] = Array.prototype.values;
  console.log("own values", collect(values));
  const notCallable = [1];
  notCallable[Symbol.iterator] = 42;
  try { collect(notCallable); } catch (e) { console.log("not callable", e.constructor.name); }
  const spread = [1, 2];
  spread[Symbol.iterator] = function* () { yield "spread"; };
  console.log("spread", [...spread].join(","), Array.from(spread).join(","));
  const orphan = [1, 2];
  Object.setPrototypeOf(orphan, null);
  try { collect(orphan); } catch (e) { console.log("null proto", e.constructor === TypeError); }
  const viaGetter = Object.create(Array.prototype);
  let receiver = null;
  Object.defineProperty(viaGetter, Symbol.iterator, { get() { receiver = this; return Array.prototype.values; } });
  const read = [3, 4];
  Object.setPrototypeOf(read, viaGetter);
  console.log("getter receiver", collect(read), receiver === read);
  console.log("restored", collect([1, 2]));
}

// Non-array iterables through the same loop shape.
console.log("string", collect("ab"));
console.log("typed", collect(new Int8Array([5, -6])));
console.log("set", collect(new Set([1, 1, 2])));
console.log("args", (function () { return collect(arguments); })(4, 5));
console.log("entries", collect([..."xy"].entries()));

// Objects stay live across GC while the loop holds them as the current value.
{
  const a = [];
  for (let i = 0; i < 200; i++) a.push({ i, tag: "o" + i });
  let total = 0;
  for (let round = 0; round < 50; round++) {
    for (const o of a) {
      const garbage = [o.i, { x: o.tag }];
      total += o.i + garbage.length;
    }
  }
  console.log("gc", total, a[199].tag);
  let doubles = 0;
  for (const x of [0.5, 1.5, -0, NaN, 2 ** 53]) doubles += Number.isNaN(x) ? 1000 : x;
  console.log("doubles", doubles);
}

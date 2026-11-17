// `k in a` answers an Array's present element inline, and a define into an
// open Array (map / filter's CreateDataPropertyOrThrow) stores inline
// (il_elem_lowering.cpp lower_has_elem, lower_elem_set in define mode). Each
// in a loop, so the site is compiled for more than one run, with the keys and
// receivers that must reach the helper.

function has(o, k) { return k in o; }
const keys = [0, 1, 2, 3, -1, 1.5, -0, "1", "length", "x", 2 ** 32, NaN];
const holey = [10, , 30];
const sub = new (class extends Array {})(); sub.push(1, 2);
const typed = new Float64Array(2);
const arrayLike = { 0: "a", length: 1 };
for (let r = 0; r < 3; ++r) {
  for (const k of keys) {
    console.log(String(k), has(holey, k), has(sub, k), has(typed, k), has(arrayLike, k), has([], k));
  }
}
const withX = [1, 2]; withX.x = 1;
const chained = [, 5]; Object.setPrototypeOf(chained, { 0: "proto" });
for (let r = 0; r < 3; ++r) console.log(has(withX, "x"), has(withX, 1), has(chained, 0), has(chained, 1), has(chained, 2));
try { has(5, 0); } catch (e) { console.log(e instanceof TypeError); }

// map / filter over sparse, holey and huge-index inputs: the result's holes and
// lengths are the define's.
const sparse = [1, , 3, , 5];
for (let r = 0; r < 3; ++r) {
  const m = sparse.map(x => x * 2);
  console.log(m.length, 1 in m, JSON.stringify(m), sparse.filter(x => x > 1).join(","));
}
class Tracked extends Array {}
const t = Tracked.from([1, 2, 3]);
for (let r = 0; r < 2; ++r) {
  const m = t.map(x => x + 1);
  console.log(m instanceof Tracked, m.join(","), t.filter(x => x & 1).constructor === Tracked);
}
const objs = [{ a: 1 }, { a: 2 }];
for (let r = 0; r < 3; ++r) console.log(objs.map(o => o.a).join(","), objs.map(o => [o]).length, objs.filter(o => o.a > 1)[0].a);

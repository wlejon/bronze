// Dense element writes that land on holes: arrays made with holes
// (`new Array(n)`, `delete`, an elision) are filled in place by the speculated
// store, a frozen and a sealed array refuse it, and once an index setter sits
// on a prototype a write into a hole of an array inheriting it must reach the
// setter, while a bare array's holes still take the element.
"use strict";
function fill(a, n, base) {
  for (let i = 0; i < n; i++) a[i] = base + i;
  return a;
}
// The same loop for the receivers the generic store must handle, kept apart
// so `fill`'s harvest is the hole fills alone.
function fillAny(a, n, base) {
  for (let i = 0; i < n; i++) a[i] = base + i;
  return a;
}
function main() {
  let total = 0;
  for (let r = 0; r < 3000; r++) {
    const a = [1, 2, 3, 4, 5, 6, 7, 8];
    if (r & 1) delete a[r & 7];
    const b = r % 3 === 0 ? new Array(8) : [0, , 0, , 0, , 0, ,];
    fill(a, 8, r);
    fill(b, 8, r);
    for (let i = 0; i < 8; i++) total += a[i] + b[i];
    if (r === 2999) console.log(a.join(","), b.join(","), Object.keys(b).length);
  }
  const frozen = Object.freeze([1, , 3]);
  try { fillAny(frozen, 3, 9); } catch (e) { console.log("frozen", e instanceof TypeError); }
  const sealed = Object.seal([1, , 3]);
  try { fillAny(sealed, 3, 9); } catch (e) { console.log("sealed", e instanceof TypeError, sealed.join(",")); }
  let seen = 0;
  const p = Object.create(Array.prototype);
  Object.defineProperty(p, 2, { set(v) { seen += v; }, configurable: true });
  const h = [0, 0, , 0];
  Object.setPrototypeOf(h, p);
  fillAny(h, 4, 100);
  const g = [0, 0, , 0];
  fillAny(g, 4, 200);
  console.log(Array.prototype.join.call(h, ","), seen, Object.keys(h).join(","), g.join(","));
  console.log(total);
}
main();

// Truthiness sites a profile sees with only Booleans, only objects and
// null / undefined, or a mix of Booleans and undefined (the join of `a && b`),
// warmed that way and then fed every other kind of value: a hint built on the
// warm-up kinds may only move code, never change an answer.
function flag(o) { return o.visible ? 1 : 0; }
function present(o) { if (o.map) return 1; return 0; }
function both(o) { return o.visible && o.on ? 1 : 0; }
function either(o) { return o.map || o.on ? 1 : 0; }
const warm = [
  { visible: true, on: true, map: {} },
  { visible: false, on: undefined, map: null },
  { visible: true, on: false, map: undefined },
];
let w = 0;
for (let i = 0; i < 3000; i++) {
  const o = warm[i % 3];
  w += flag(o) + present(o) + both(o) + either(o);
}
console.log(w);
const all = [
  0, -0, 1, -1, 0.5, NaN, Infinity, 5e-324, 0 | 0, 7 | 0, 2 ** 31,
  0n, 1n, 2n ** 70n, "", "0", "a", true, false, null, undefined,
  {}, [], function () {}, Symbol("s"), new Boolean(false),
];
let s = "";
for (const v of all) {
  const o = { visible: v, on: v, map: v };
  s += `${flag(o)}${present(o)}${both(o)}${either(o)} `;
  const p = { visible: true, on: v, map: null };
  s += `${both(p)}${either(p)}|`;
}
console.log(s);

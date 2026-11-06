// for-in's key list: own then inherited, integer-like keys first ascending,
// shadowed and non-enumerable keys skipped, keys added during the walk not
// visited — over shapes the enumeration cache has already seen.
function keysOf(o) { const out = []; for (const k in o) out.push(k); return out.join(","); }
const proto = { inherited: 1, shadow: 2 };
Object.defineProperty(proto, "hiddenProto", { value: 3, enumerable: false });
function make() {
  const o = Object.create(proto);
  o.b = 1; o[10] = 2; o.a = 3; o[2] = 4; o.shadow = 5; o["01"] = 6;
  Object.defineProperty(o, "hidden", { value: 7, enumerable: false });
  return o;
}
for (let i = 0; i < 3; i++) console.log(keysOf(make()));
// A non-enumerable own property hides an enumerable inherited one.
const masked = Object.create(proto);
Object.defineProperty(masked, "inherited", { value: 0, enumerable: false });
console.log(keysOf(masked));
// Keys added during the walk are not visited.
for (let i = 0; i < 2; i++) {
  const o = { p: 1, q: 2 };
  const seen = [];
  for (const k in o) { seen.push(k); o["n" + seen.length] = 0; }
  console.log(seen.join(","), Object.keys(o).length);
}
// The prototype changes between walks of the same shape.
const o2 = Object.create(proto);
o2.own = 1;
console.log(keysOf(o2));
proto.later = 1;
console.log(keysOf(o2));
delete proto.later;
console.log(keysOf(o2));
Object.setPrototypeOf(o2, { other: 1 });
console.log(keysOf(o2));
// Deleting a key that was already visited, and one on the prototype that the
// receiver shadows.
const o3 = Object.create(proto);
o3.shadow = 1; o3.m = 2;
const seen3 = [];
for (const k in o3) { seen3.push(k); if (k === "m") delete o3.shadow; }
console.log(seen3.join(","));

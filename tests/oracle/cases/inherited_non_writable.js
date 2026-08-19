// 10.1.9.2 OrdinarySetWithOwnDescriptor step 2: a write walks the prototype
// chain looking for the property, and an INHERITED non-writable data property
// refuses the write outright — it does not create a shadowing own property.
// Sloppy code discards the write silently, strict code throws (6.2.5.5
// PutValue step 6). Frozen prototypes are the common way to reach it, and
// treating the write as an ordinary shadow made a frozen prototype no
// protection at all.
const proto = {};
Object.defineProperty(proto, "locked", { value: 1, writable: false, enumerable: true });
proto.open = 2;

const child = Object.create(proto);
child.locked = 99;
console.log(child.locked, Object.prototype.hasOwnProperty.call(child, "locked"));

// The writable sibling shadows exactly as it always did.
child.open = 42;
console.log(child.open, proto.open, Object.prototype.hasOwnProperty.call(child, "open"));

// An OWN non-writable property refuses the same way.
const own = {};
Object.defineProperty(own, "fixed", { value: 5, writable: false });
own.fixed = 6;
console.log(own.fixed);

// A frozen prototype refuses every one of its properties.
const frozenProto = Object.freeze({ a: 1, b: 2 });
const under = Object.create(frozenProto);
under.a = 10;
under.b = 20;
console.log(under.a, under.b, Object.getOwnPropertyNames(under).length);

// Strict code throws instead of discarding, and names the property.
function strictWrite() {
  "use strict";
  const o = Object.create(proto);
  o.locked = 7;
}
try {
  strictWrite();
  console.log("no throw");
} catch (e) {
  console.log(e.name);
}

// A SETTER further up the chain still runs — the refusal is for data
// properties, and step 2 only reaches it after the accessor case.
let wrote = 0;
const accessorProto = { set s(v) { wrote = v; } };
const viaSetter = Object.create(accessorProto);
viaSetter.s = 3;
console.log(wrote, Object.prototype.hasOwnProperty.call(viaSetter, "s"));

// Two levels up is the same answer as one.
const deep = Object.create(Object.create(proto));
deep.locked = 11;
console.log(deep.locked);

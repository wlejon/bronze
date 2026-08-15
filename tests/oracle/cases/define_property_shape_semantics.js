// Pins ECMA-262 10.1.6.3 ValidateAndApplyPropertyDescriptor through the
// shape-preserving defineProperty path: descriptors that shapes can carry
// stay in shape-land (the three.js Object3D constructor pattern), and the
// attribute rules hold exactly as they did when every definition degraded to
// dictionary mode. Expectations derived from the spec, not from any engine.

// writable true -> false on a NON-configurable property is one of the two
// changes 10.1.6.3 still permits when configurable is false — and a shared
// shape cannot express it, so this is the demotion seam.
const a = {};
Object.defineProperty(a, 'x', {value: 1, writable: true, enumerable: true, configurable: false});
Object.defineProperty(a, 'x', {writable: false});
a.x = 99; // sloppy mode: silent no-op against a now-read-only property
console.log(a.x);
console.log(Object.getOwnPropertyDescriptor(a, 'x').writable);

// An accessor descriptor's missing attributes default to FALSE (6.2.6.5) —
// unlike a literal `get g() {}`, which is born configurable.
const b = {};
Object.defineProperty(b, 'g', {get: function () { return 7; }});
console.log(b.g);
const gd = Object.getOwnPropertyDescriptor(b, 'g');
console.log(gd.configurable);
console.log(gd.enumerable);
console.log(delete b.g);
console.log(b.g);

// Data descriptor defaults: {value: 3} means non-writable, non-enumerable,
// non-configurable.
const c = {};
Object.defineProperty(c, 'z', {value: 3});
console.log(Object.keys(c).length);
c.z = 5;
console.log(c.z);
console.log(JSON.stringify(c));

// Redefining a non-configurable non-writable data property throws for a
// DIFFERENT value and is a no-op for the SAME value (SameValue, 10.1.6.3).
let threw = false;
try { Object.defineProperty(c, 'z', {value: 4}); } catch (e) { threw = true; }
console.log(threw);
threw = false;
try { Object.defineProperty(c, 'z', {value: 3}); } catch (e) { threw = true; }
console.log(threw);

// The hot shape: defineProperty/defineProperties in a constructor, then
// reads at loop heat — the accesses the mechanism exists to keep on the
// inline caches. The string concat allocates, so the suite's GC-stress
// re-run collects mid-construction while instances are live.
function Node3(i) {
  Object.defineProperty(this, 'id', { value: i });
  Object.defineProperties(this, {
    pos: { value: 'p' + i, writable: true, enumerable: true, configurable: true },
    vis: { value: true, writable: true, enumerable: true, configurable: true }
  });
}
let idSum = 0;
let visCount = 0;
let lastPos = '';
for (let i = 0; i < 500; i = i + 1) {
  const n = new Node3(i);
  idSum = idSum + n.id;
  if (n.vis) visCount = visCount + 1;
  lastPos = n.pos;
}
console.log(idSum);
console.log(visCount);
console.log(lastPos);
console.log(Object.keys(new Node3(1)).join(','));

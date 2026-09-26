// Property sites that go polymorphic and then megamorphic after tier-up
// (docs/il2mir.md, section 7): a read's way-0 test and its scan of the
// site's other ways are two speculation sites, so a hot read that saw one
// shape is pinned to way 0, deoptimizes when a second shape arrives, and is
// re-optimized with the scan guarded; past the site's ways the scan's guard
// fails too and the helper takes over. A loop at the top level runs the same
// transitions in OSR code, whose repeated deopts must retire its entry
// rather than re-enter it from the Tier-0 frame each one resumes in.

function shapes(n) {
  const out = [];
  for (let s = 0; s < n; s++) {
    const o = {};
    o['lead' + s] = s;
    o.x = s + 1;
    o.y = 10 * s;
    out.push(o);
  }
  return out;
}

function readXY(o) {
  return o.x + o.y;
}
function writeX(o, v) {
  o.x = v;
  return o.x;
}
function callM(o) {
  return o.m();
}

function phase(objs, reps) {
  let acc = 0;
  for (let r = 0; r < reps; r++) {
    for (let i = 0; i < objs.length; i++) acc += readXY(objs[i]);
  }
  return acc;
}

// Mono, then 2, 4 (the site's ways) and 8 shapes through the same reads.
for (const n of [1, 2, 4, 8, 2, 1]) {
  console.log('read', n, phase(shapes(n), 400));
}

// Receivers the inline paths refuse: primitives, an accessor, a property on
// the prototype, a dictionary-mode object, and an absent key.
const proto = { get x() { return 7; }, y: 1 };
const inherited = Object.create({ x: 3, y: 4 });
const dict = { x: 1, y: 2, z: 3 };
delete dict.z;
const odd = [Object.create(proto), inherited, dict, { y: 5 }, { x: 'a', y: 'b' }];
let odds = '';
for (let r = 0; r < 300; r++) odds = String(readXY(odd[r % odd.length]));
console.log('odd', odds, readXY(odd[3]), readXY(odd[4]));
let strLen = 0;
const strs = ['a', 'bb', 'ccc'];
for (let r = 0; r < 600; r++) strLen += strs[r % 3].length;
console.log('length', strLen);

// Writes: one shape, then several.
let w = 0;
const ws = shapes(6);
for (let r = 0; r < 3000; r++) w += writeX(ws[r < 1500 ? 0 : r % 6], r);
console.log('write', w, ws.map((o) => o.x).join(','));

// Method reads through a polymorphic receiver.
class A { m() { return 1; } }
class B { m() { return 2; } }
class C extends A { m() { return 3; } }
const recv = [new A(), new B(), new C(), { m() { return 4; } }, { k: 0, m() { return 5; } }];
let calls = 0;
for (let r = 0; r < 4000; r++) calls += callM(recv[r < 2000 ? 0 : r % recv.length]);
console.log('method', calls);

// The same transitions in one top-level loop, entered through OSR.
const all = shapes(8);
let top = 0;
for (let i = 0; i < 60000; i++) {
  const o = all[i < 20000 ? 0 : i < 40000 ? i % 3 : i % 8];
  top += o.x * 2 + o.y;
}
console.log('osr', top);

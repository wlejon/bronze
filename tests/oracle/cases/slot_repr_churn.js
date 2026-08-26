// Stage R1's storage model, from a program's side.
//
// Nothing here mentions a representation, and that is the point: whether a slot
// holds a raw double or a boxed Value must be invisible. Under
// BRONZE_SLOT_REPR_OBSERVED=1 every field below is born a double slot and the
// non-number stores generalize; under the default only a --pins manifest can do
// that, and there is none; under BRONZE_NO_SLOT_REPR=1 no slot is ever a
// double. The three runs must print the same bytes.

class Vec {
  constructor(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  }
}

// Seven fields, so three of them are past the four inline slots and live in the
// out-of-line block — the half of the storage `kInlineSlots` does not reach.
class Wide {
  constructor(n) {
    this.a = n;
    this.b = n + 1;
    this.c = n + 2;
    this.d = n + 3;
    this.e = n + 4;
    this.f = n + 5;
    this.g = n + 6;
  }
}

function line(label, o, keys) {
  let out = label + ':';
  for (let i = 0; i < keys.length; i++) {
    out += ' ' + keys[i] + '=' + o[keys[i]];
  }
  return out;
}

const vk = ['x', 'y', 'z'];
const wk = ['a', 'b', 'c', 'd', 'e', 'f', 'g'];

// 1. Two objects that share a shape; one of them takes a non-number and the
//    other must not notice.
const a = new Vec(1.5, 2.5, 3.5);
const b = new Vec(4.5, 5.5, 6.5);
a.y = 'not a number';
console.log(line('a', a, vk));
console.log(line('b', b, vk));
console.log('shape-mates independent:', b.y === 5.5, typeof a.y, typeof b.y);

// 2. A number goes back in. The slot does not become a double again, and the
//    value is the value either way.
a.y = 99.25;
console.log(line('a-again', a, vk), 'typeof', typeof a.y);

// 3. Every non-number kind, one at a time, over a fresh pair each time.
const kinds = [null, undefined, true, false, 'str', {k: 1}, [1, 2], Symbol('s')];
for (let i = 0; i < kinds.length; i++) {
  const p = new Vec(i, i + 0.5, i + 0.25);
  const q = new Vec(i, i + 0.5, i + 0.25);
  p.z = kinds[i];
  const shown = typeof p.z === 'symbol' ? 'symbol' : String(p.z);
  console.log('kind', i, typeof p.z, shown, 'mate', q.z);
}

// 4. The out-of-line half, with a generalization in the middle of it.
const w = new Wide(10);
const w2 = new Wide(10);
console.log(line('w', w, wk));
w.f = 'six';
w.b = null;
console.log(line('w-mixed', w, wk));
console.log(line('w2', w2, wk));

// 5. A double whose bits are those of a small integer — the pattern a slot the
//    collector must not trace is built out of. Allocate hard around it so a
//    collection runs while it is live.
const alias = new Vec(5e-324, 1.5e-323, 2.5e-323);
let sink = 0;
for (let i = 0; i < 60000; i++) {
  const t = new Vec(i * 0.5, i * 0.25, i * 0.125);
  sink += t.x - t.y - t.z;
  if ((i & 8191) === 0) {
    const junk = [];
    for (let j = 0; j < 200; j++) junk.push(new Wide(j));
    sink += junk[13].g * 1e-9;
  }
}
console.log(line('alias', alias, vk));
console.log('sink', Math.round(sink * 1000) / 1000);

// 6. defineProperty, freeze and delete over slots that were doubles.
const d = new Vec(7.5, 8.5, 9.5);
Object.defineProperty(d, 'x', {value: 'defined', writable: true, enumerable: true,
                               configurable: true});
console.log(line('d', d, vk), Object.keys(d).join(','));
delete d.y;
console.log('after delete:', d.y, line('d', d, vk), Object.keys(d).join(','));
// Freezing is checked by what it REFUSES rather than by whether the refusal
// throws: strict and sloppy answer that differently, and this case has to read
// the same in both.
const fr = new Vec(1.25, 2.25, 3.25);
Object.freeze(fr);
console.log(line('fr', fr, vk), Object.isFrozen(fr),
            Object.getOwnPropertyDescriptor(fr, 'x').writable);

// 7. Spread, assign and JSON over a mix of double and generalized slots.
const src = new Vec(1.5, 2.5, 3.5);
src.z = 'z-string';
const spread = {...src};
const assigned = Object.assign({}, src);
console.log('spread', JSON.stringify(spread));
console.log('assigned', JSON.stringify(assigned));
console.log('json', JSON.stringify(src), JSON.stringify(new Wide(1)));

// 8. NaN, the infinities and the signed zero, which is where a representation
//    that canonicalizes could lose something.
const s = new Vec(NaN, Infinity, -0);
console.log('special', s.x, s.x !== s.x, s.y, 1 / s.z, Object.is(s.z, -0));
s.y = -Infinity;
console.log('special2', s.y, Object.is(1 / s.y, -0));

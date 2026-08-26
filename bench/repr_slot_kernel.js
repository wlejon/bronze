// Stage R1: the per-slot representation, hammered.
//
// Every field below is pinned `number` (bench/pins/repr-slot-kernel.pins), so
// under BRONZE_NO_SLOT_REPR unset every one of them is born a DOUBLE slot and
// every store in the loop goes through the representation store path. With the
// seam set they are ordinary boxed slots and this is the same program on the
// storage model bronze had before the stage. The checksum must be identical.
//
// `Bag` is deliberately not `Vec`: one class whose four fields all fit the
// inline slots, one whose fields spill into the out-of-line block, so both
// halves of `ObjectHeader::setSlot` and both halves of the collector's
// double-slot skip are exercised.

class Vec {
  constructor(x, y, z, w) {
    this.x = x;
    this.y = y;
    this.z = z;
    this.w = w;
  }
}

class Bag {
  constructor(seed) {
    this.a = seed;
    this.b = seed * 2;
    this.c = seed * 3;
    this.d = seed * 4;
    this.e = seed * 5;
    this.f = seed * 6;
    this.g = seed * 7;
  }
}

function run(iterations) {
  const v = new Vec(1.5, 2.5, 3.5, 4.5);
  const bag = new Bag(0.25);
  // A second live instance of each shape, never written in the loop: it is what
  // proves a generalization elsewhere cannot disturb an object that shares the
  // shape, and it keeps the collector walking a mix of double and boxed slots.
  const pinnedVec = new Vec(9.5, 8.5, 7.5, 6.5);
  const pinnedBag = new Bag(1.25);

  let acc = 0;
  for (let i = 0; i < iterations; i++) {
    const t = i * 0.5;
    v.x = t + 1.0;
    v.y = v.x * 2.0;
    v.z = v.y - v.x;
    v.w = v.z + v.y;

    bag.a = v.x;
    bag.b = v.y + bag.a;
    bag.c = v.z + bag.b;
    bag.d = v.w + bag.c;
    bag.e = bag.d * 0.5;
    bag.f = bag.e + bag.a;
    bag.g = bag.f - bag.b;

    acc += v.w + bag.g + pinnedVec.x + pinnedBag.g;
    // Allocation, so a collection runs over a heap that holds objects with
    // double slots inline (Vec) and out of line (Bag).
    if ((i & 1023) === 0) {
      const scratch = new Vec(t, t, t, t);
      acc += scratch.w * 1e-6;
    }
  }
  const checksum = Math.round(acc * 1000);
  console.log('repr_slot checksum=' + checksum);
}

run(400000);

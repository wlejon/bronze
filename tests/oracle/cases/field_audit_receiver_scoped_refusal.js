// What a computed write costs when the pass CAN see whose object it lands on.
//
// `cases/field_audit_computed_receiver_guess.js` pins that an unanalyzable
// `recv[k] = v` may never be waved through. This case pins the other edge of
// the same rule: how far the refusal it forces is allowed to reach.
//
// The audit certifies NAMES — "nothing anywhere puts a non-number in `x`" — so
// a site it cannot analyse used to have only one answer, which was to refuse
// every name in the program. The answer now is the receiver's TYPE. A receiver
// this compilation watched being made is an instance of one shape class and of
// nothing else, so the refusal is recorded against that class and the classes
// its `extends` family reaches, and every other class keeps what it proved.
// A receiver typed as a real array is not an instance of any declared class at
// all, so a write through one costs the declared classes nothing.
//
// Nothing here is observable as a type — a refusal that is too wide only makes
// code slower. What IS observable is the correctness the refusal buys, so every
// section below stores something the field's claim says cannot be there and
// reads it back. `--no-infer` must print the same bytes, which is the whole
// point: these are the values the language specifies.
//
// The reads are written as merges (a loop) for the reason
// `field_audit_numeric_spelling.js` gives: a field type only becomes a hard
// unbox where lowering has to name a block parameter's type.

function show(label, v) {
  console.log(label + "=" + v + " (" + typeof v + ")");
}

class V3 {
  constructor() {
    this.x = 0;
    this.y = 0;
  }
  sum() {
    let s = this.x + this.y;
    for (let i = 0; i < 2; i++) s = this.x + this.y;
    return s;
  }
}

// The same field names, a different class. Nothing below writes anything but a
// number into one of these.
class C3 {
  constructor() {
    this.x = 0;
    this.y = 0;
  }
  sum() {
    let s = this.x + this.y;
    for (let i = 0; i < 2; i++) s = this.x + this.y;
    return s;
  }
}

// Spelled nowhere as a literal the pass can follow to a site, so every write
// under it is genuinely computed.
const kk = ["x"][0];

// ---- 1. a receiver whose class is known ---------------------------------
//
// `a` came out of `new V3()`, so the write reaches V3 instances and no others:
// V3's fields lose their claims and C3's keep theirs.

const a = new V3();
a[kk] = "hi";
a.y = 1;
show("V3 clobbered", a.sum());

const b = new C3();
b.x = 2;
b.y = 3;
show("C3 intact", b.sum());

// ---- 2. a real array of objects -----------------------------------------
//
// The receiver is an array by TYPE, not because of what it is called. The write
// lands on an element — here through the STRING "0", which is the same property
// as the index 0 — and the objects the array holds are untouched.

const objs = [new C3(), new C3()];
objs[1].x = 4;
objs[1].y = 6;
const idx = ["0"][0];
objs[idx] = "clobbered";
show("elem0", objs[0]);
show("elem1", objs[1].sum());

// ---- 3. a string key and a numeric key through one site -----------------
//
// One write, run twice: once under a name a class claims a slot for, once under
// an index no layout describes. The first lands in V3's `y`, the second creates
// an ordinary "0" property on the same object.

const dual = new V3();
const twoKeys = ["y", 0];
for (let i = 0; i < 2; i++) dual[twoKeys[i]] = "K" + i;
show("dual.y", dual.y);
show("dual[0]", dual[0]);
show("dual.sum", dual.sum());

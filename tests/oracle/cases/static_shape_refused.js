// The edge of the layout analysis: constructs it refuses, and constructs that
// LOOK like refusals and are not.
//
// A refusal is a decision not to CLAIM a slot; it is never a change of
// behaviour. Each block below is ordinary JavaScript whose answer is fixed by
// the language, and the point of the case is that the answer does not move
// either way — whether the class is modelled or refused, and whether the
// mechanism is on or off (BRONZE_NO_STATIC_SHAPES).
//
// Blocks 2, 3, 4, 6 and 7 are the ones that are NOT refusals, each for a reason
// worth pinning: a define with a data descriptor is a shape transition like any
// other; a field a method installs lands after every constructor field and so
// moves none of them; a computed write either hits a slot or appends; an
// if/else that assigns the same name on both paths assigns it on every path;
// and a constructor whose LAST statement calls such a method has nothing after
// the append to displace.

// 1. A constructor that assigns conditionally on one path only: two instances,
//    two orders, so no one layout describes them. REFUSED.
class Cond {
  constructor(withY) {
    this.x = 1;
    if (withY) this.y = 2;
    this.z = 3;
  }
}
const c1 = new Cond(false);
const c2 = new Cond(true);
console.log(Object.keys(c1).join(","));
console.log(Object.keys(c2).join(","));
console.log(c1.z + c2.z);
console.log(String(c1.y));

// 2. A constructor that defines a data property on `this`. Not a refusal: the
//    runtime routes a new key on a shaped object through the ordinary
//    transition, so `id` takes the next slot and `tail` the one after.
let nextId = 0;
class Reflected {
  constructor() {
    this.kind = "r";
    Object.defineProperty(this, "id", { value: nextId++, enumerable: true });
    this.tail = 9;
  }
}
const r1 = new Reflected();
const r2 = new Reflected();
console.log(r1.id + "," + r2.id);
console.log(Object.keys(r2).join(","));
console.log(r2.kind + r2.tail);

// 3. A method that installs a field the constructor does not. Not a refusal:
//    `extra` appends after `n`, which is where the layout already ended.
class Lazy {
  constructor() {
    this.n = 0;
  }
  bump() {
    this.extra = (this.extra || 0) + 1;
    this.n++;
    return this.extra;
  }
}
const lz = new Lazy();
console.log(lz.bump() + "," + lz.bump());
console.log(Object.keys(lz).join(","));
console.log(lz.n);

// 4. A computed write on `this`. Not a refusal: the name is a run-time fact,
//    but the write appends or hits, and `base` keeps slot 0 either way.
class Keyed {
  constructor(k) {
    this.base = 1;
    this[k] = 2;
  }
}
const k1 = new Keyed("p");
const k2 = new Keyed("q");
console.log(Object.keys(k1).join(",") + "|" + Object.keys(k2).join(","));

// 5. `extends` something that is not a class declaration: the base's own field
//    set belongs to code this analysis did not model as a layout. REFUSED.
function Base(v) {
  this.v = v;
}
class Derived extends Base {
  constructor(v) {
    super(v);
    this.w = v + 1;
  }
}
const d = new Derived(3);
console.log(d.v + "," + d.w);
console.log(Object.keys(d).join(","));

// A proven class declared AFTER all of the above still proves: a refusal is
// per class, not a switch for the program.
class Ok {
  constructor() {
    this.u = 5;
    this.t = 6;
  }
  total() {
    return this.u + this.t;
  }
}
let acc = 0;
for (let i = 0; i < 300; i++) acc += new Ok().total();
console.log(acc);
console.log(Object.keys(new Ok()).join(","));

// 6. An if/else that assigns the same field on both paths. Not a refusal: the
//    field is installed on every path, at the same position on every path.
class Both {
  constructor(flag) {
    this.a = 1;
    if (flag) {
      this.mode = "on";
    } else {
      this.mode = "off";
    }
    this.b = 2;
  }
}
const b1 = new Both(true);
const b2 = new Both(false);
console.log(Object.keys(b1).join(",") + "|" + Object.keys(b2).join(","));
console.log(b1.mode + b2.mode + (b1.b + b2.b));

// 7. A constructor whose last statement calls a method that installs a field,
//    and a subclass of it. The base proves; the subclass writes its own field
//    after the append, so it refuses — and both answer the same.
class TailBase {
  constructor(withExtra) {
    this.p = 1;
    this.q = 2;
    this.install(withExtra);
  }
  install(withExtra) {
    if (withExtra) this.later = 3;
  }
}
class TailDerived extends TailBase {
  constructor(withExtra) {
    super(withExtra);
    this.r = 4;
  }
}
const t1 = new TailBase(false);
const t2 = new TailBase(true);
const t3 = new TailDerived(true);
console.log(Object.keys(t1).join(",") + "|" + Object.keys(t2).join(","));
console.log(Object.keys(t3).join(","));
let tsum = 0;
for (let i = 0; i < 300; i++) tsum += t3.p + t3.q + t3.r;
console.log(tsum);

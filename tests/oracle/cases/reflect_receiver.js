// `Reflect.get` and `Reflect.set` with the RECEIVER argument (28.1.6 step 4,
// 28.1.13 step 5) — the third and fourth parameters, which decide what `this` an
// accessor runs against and, for a data property, where the write lands.
//
// Two rules, and the second is the one that surprises. An ACCESSOR reached
// through the target's chain runs with the receiver as `this` (10.1.9.2 step 3),
// which is what makes `Reflect.get(proto, 'v', instance)` read the instance's
// fields. A DATA property does not move at all: step 2 creates the property on
// the RECEIVER and leaves the target alone, so `Reflect.set(o, 'a', 9, other)`
// gives `other` an `a` and `o.a` keeps its old value. That is the mechanism a
// `super.x = v` in a class relies on, spelled explicitly.
//
// The boolean each `Reflect.set` returns is the [[Set]] result and is pinned
// beside the effect: step 2 refuses a non-writable target property, a
// setter-less accessor, a primitive receiver, and a receiver whose own property
// is an accessor or non-writable — four false answers that are not exceptions.
//
// A PROXY target takes none of that route: 10.5.9 hands the receiver to the
// `set` trap as its fourth argument and the trap decides everything, which the
// proxy block pins from inside the trap.

// ---- an accessor runs against the receiver --------------------------------
const src = { tag: "T", get who() { return this.tag; } };
console.log(Reflect.get(src, "who", { tag: "R" }), Reflect.get(src, "who"));

const proto = { get doubled() { return this.n * 2; } };
const inst = Object.create(proto);
inst.n = 5;
console.log(Reflect.get(proto, "doubled", inst), Reflect.get(inst, "doubled"));

// A DATA property is the same value whatever the receiver: only an accessor
// reads `this`.
console.log(Reflect.get({ a: 1 }, "a", { a: 2 }));

// A static accessor's receiver is the class, or whatever is passed instead.
class C { static get s() { return this.tag; } }
C.tag = "own";
console.log(Reflect.get(C, "s", { tag: "X" }), Reflect.get(C, "s"));

// ---- a setter runs against the receiver ----------------------------------
const store = { set x(v) { this.seen = v; } };
const sink = {};
console.log(Reflect.set(store, "x", 42, sink), sink.seen, "seen" in store);

// ---- a data write lands on the RECEIVER (10.1.9.2 step 2) ----------------
const owner = { a: 1 };
const dest = {};
console.log(Reflect.set(owner, "a", 9, dest), dest.a, owner.a);

// Absent from the whole chain: step 1.d invents a writable data descriptor, so
// the write still creates the property on the receiver.
const bare = Object.create(null);
const fresh = {};
console.log(Reflect.set(bare, "k", 7, fresh), fresh.k, "k" in bare);

// With no receiver the write is an ordinary one and lands on the target.
const plain = { a: 1 };
console.log(Reflect.set(plain, "a", 2), plain.a);

// ---- the four false answers ---------------------------------------------
console.log(Reflect.set(Object.freeze({ a: 1 }), "a", 2, {}));
console.log(Reflect.set({ get g() { return 1; } }, "g", 2, {}));
const rigid = Object.freeze({ a: 0 });
console.log(Reflect.set({ a: 1 }, "a", 2, rigid), rigid.a);
console.log(Reflect.set({ a: 1 }, "a", 2, 5));
let ran = false;
const accReceiver = { set a(v) { ran = true; } };
console.log(Reflect.set({ a: 1 }, "a", 2, accReceiver), ran);

// ---- a proxy target: the trap gets the receiver -------------------------
const marker = { m: 1 };
const pget = new Proxy({}, {
  get(target, key, receiver) { return key + ":" + (receiver === marker); },
});
console.log(Reflect.get(pget, "tag", marker), Reflect.get(pget, "tag"));

const pset = new Proxy({}, {
  set(target, key, value, receiver) { receiver.landed = key + "=" + value; return true; },
});
const caught = {};
console.log(Reflect.set(pset, "k", 7, caught), caught.landed);

// ---- step 1: a primitive target has no internal method to call ----------
function typeErrorOf(f) {
  try { f(); return "no-throw"; } catch (e) { return e instanceof TypeError ? "TypeError" : "WRONG"; }
}
console.log(typeErrorOf(() => Reflect.get(5, "x")), typeErrorOf(() => Reflect.set(null, "x", 1)));

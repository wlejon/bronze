// An accessor's guard (accessor.ic.is) may reuse the receiver's shape that an
// earlier guard on the same value computed (il_shape_memo.cpp). Each block
// below warms a site on one class and then hands it a receiver, or does
// something between the two property reads, that would make a stale shape
// answer wrongly: a getter that reshapes `this`, swaps its prototype,
// redefines the accessor, throws; a generator suspended between the reads;
// receivers that are not plain objects at all.

class C {
  constructor(a) { this.a = a; }
  get g() { return "G" + this.a; }
  set s(v) { this.a = v * 10; }
  get h() { return "H" + this.a; }
}

function readAG(o) { const x = o.a; const y = o.g; return x + ":" + y; }
function readGH(o) { const x = o.g; const y = o.h; return x + ":" + y; }
function writeAS(o, v) { o.a = v; o.s = v + 1; return o.a; }

let acc = "";
for (let i = 0; i < 200; i++) {
  acc = readAG(new C(i)) + "|" + readGH(new C(i)) + "|" + writeAS(new C(0), i);
}
console.log("warm " + acc);

// 1. `a` is a getter that gives the receiver an own data `g`.
const p1 = Object.create(C.prototype);
Object.defineProperty(p1, "a", {
  configurable: true,
  get() { Object.defineProperty(this, "g", { value: "own", configurable: true }); return 7; },
});
console.log("1 " + readAG(p1));

// 2. `a` is a getter that swaps the receiver's prototype.
const altProto = { get g() { return "alt"; } };
const p2 = new C(3);
Object.defineProperty(p2, "a", { configurable: true, get() { Object.setPrototypeOf(this, altProto); return 5; } });
console.log("2 " + readAG(p2));

// 3. `g` reshapes `this` and redefines `h` on the receiver before `h` is read.
const p3 = new C(4);
Object.defineProperty(p3, "g", {
  configurable: true,
  get() { this.extra = 1; Object.defineProperty(this, "h", { value: "shadow", configurable: true }); return "g3"; },
});
console.log("3 " + readGH(p3));

// 4. `g` deletes `a` before `h` reads it through `this`.
const p4 = new C(9);
Object.defineProperty(p4, "g", { configurable: true, get() { delete this.a; return "g4"; } });
console.log("4 " + readGH(p4));

// 5. A frozen receiver of the warm class: the accessors still run.
const p5 = Object.freeze(new C(11));
console.log("5 " + readAG(p5) + " " + readGH(p5));

// 6. `g` throws; the next call on a plain receiver is unaffected.
const p6 = new C(1);
Object.defineProperty(p6, "g", { configurable: true, get() { throw new Error("boom" + this.a); } });
try { readAG(p6); console.log("6 no throw"); } catch (e) { console.log("6 " + e.message); }
console.log("6b " + readAG(new C(2)));

// 7. The prototype's accessor is replaced by a data property, then restored.
const savedG = Object.getOwnPropertyDescriptor(C.prototype, "g");
Object.defineProperty(C.prototype, "g", { value: "data", configurable: true, writable: true });
console.log("7 " + readAG(new C(6)));
Object.defineProperty(C.prototype, "g", savedG);
console.log("7b " + readAG(new C(6)));

// 8. The setter's prototype is replaced mid-sequence by `a`'s setter.
const p8 = new C(0);
let shadowSet = "none";
Object.defineProperty(p8, "a", {
  configurable: true,
  get() { return "a8"; },
  set(v) { Object.setPrototypeOf(this, { set s(w) { shadowSet = "alt" + w; } }); },
});
console.log("8 " + writeAS(p8, 2) + " " + shadowSet);

// 9. Receivers that are not plain objects.
const arr = [1, 2];
arr.a = "arr";
Object.defineProperty(arr, "g", { get() { return "arrG" + this.length; } });
console.log("9a " + readAG(arr));
function fnRecv() {}
fnRecv.a = "fn";
Object.defineProperty(fnRecv, "g", { get() { return "fnG" + this.name; } });
console.log("9b " + readAG(fnRecv));
const prox = new Proxy(new C(8), { get(t, k, r) { return k === "g" ? "proxG" : Reflect.get(t, k, r); } });
console.log("9c " + readAG(prox));
console.log("9d " + readAG(Object.create(new C(12))));

// 10. A generator suspended between the two reads while the receiver changes.
function* genAG(o) { const x = o.a; yield x; const y = o.g; yield y; }
for (let i = 0; i < 100; i++) { const it = genAG(new C(i)); it.next(); it.next(); }
const p10 = new C(21);
const it10 = genAG(p10);
const r10a = it10.next().value;
Object.defineProperty(p10, "g", { value: "late", configurable: true });
const r10b = it10.next().value;
console.log("10 " + r10a + " " + r10b);

// 11. A re-entrant getter: `g` calls readAG on another receiver of the class.
const p11 = new C(30);
Object.defineProperty(p11, "g", { configurable: true, get() { return "[" + readAG(new C(31)) + "]"; } });
console.log("11 " + readAG(p11));

// 12. The warm path still answers after all of the above.
console.log("12 " + readAG(new C(40)) + " " + readGH(new C(41)) + " " + writeAS(new C(0), 4));

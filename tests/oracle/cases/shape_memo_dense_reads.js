// Vector3.applyMatrix4's shape: reads of `this`, a read of `m.elements`,
// constant-index reads of that array behind one dense-array guard, then stores
// into `this`. The proven element reads leave the receiver's shape memo
// standing (il2mir/il_shape_memo.cpp, il_property_lowering.cpp), so the stores
// compare their cells with the memo instead of reading `this`'s shape again.
// What can move `this`'s shape between the reads and the stores is the
// `elements` read (a getter), never an element read: these cases reshape,
// freeze, re-prototype and accessorize `this` from that getter, and feed holes
// and strings through the element reads.
class V {
  constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
  apply(m) {
    const x = this.x, y = this.y, z = this.z;
    const e = m.elements;
    const w = 1 / (e[3] * x + e[7] * y + e[11] * z + e[15]);
    this.x = (e[0] * x + e[4] * y + e[8] * z + e[12]) * w;
    this.y = (e[1] * x + e[5] * y + e[9] * z + e[13]) * w;
    this.z = (e[2] * x + e[6] * y + e[10] * z + e[14]) * w;
    return this;
  }
}

function mat() { return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1]; }
class M { constructor() { this.elements = mat(); } }
function show(v) { return [v.x, v.y, v.z].map(String).join(","); }

const plain = new M();
let warm = "";
for (let i = 0; i < 3; i++) warm = show(new V(1, 2, 3).apply(plain));
console.log("plain", warm);

const log = [];
function adversary(act) {
  const v = new V(1, 2, 3);
  const m = { get elements() { act(v); return mat(); } };
  try {
    v.apply(m);
  } catch (err) {
    log.push(err.name);
  }
  return v;
}

const reshaped = adversary((v) => { delete v.y; v.q = 1; });
console.log("reshape", JSON.stringify(reshaped), Object.keys(reshaped).join(","));

const frozen = adversary((v) => Object.freeze(v));
console.log("freeze", JSON.stringify(frozen), log.join(","));

const P = { set x(val) { log.push("set x " + val); }, get x() { return "px"; } };
const reproto = adversary((v) => { delete v.x; Object.setPrototypeOf(v, P); });
console.log("proto", JSON.stringify(reproto), reproto.x, log.join(","));

const accessor = adversary((v) => {
  Object.defineProperty(v, "z", {
    get() { return "g"; }, set(val) { log.push("set z " + val); }, configurable: true, enumerable: true,
  });
});
console.log("accessor", JSON.stringify(accessor), log.join(","));

// The element reads themselves: a hole reads undefined, a string concatenates.
const holed = new M();
delete holed.elements[12];
console.log("hole", show(new V(1, 2, 3).apply(holed)));
const stringy = new M();
stringy.elements[13] = "5";
console.log("string", show(new V(1, 2, 3).apply(stringy)));
console.log("after", show(new V(1, 2, 3).apply(plain)));

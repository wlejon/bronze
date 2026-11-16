// A computed read whose site names many (shape, key) pairs stops filling its
// cell and asks the thread's computed-read table from generated code
// (il_prop_stubs.cpp build_elem_get_stub). Trained on many pairs of Number,
// boolean and string keys (own, on the prototype, absent, behind a getter);
// late rounds change each answer: a write, a delete, an add on the prototype,
// a getter swapped in, a key string made afresh (and collected), -0.
class Base { get g() { return this.n * 2; } }
Base.prototype.inherited = "i";
function make(i) {
    const o = i % 3 === 0 ? new Base() : {};
    o.n = i;
    o["k" + (i % 7)] = i;
    o[i % 5] = "num" + i;
    o[true] = "t";
    return o;
}
const objs = [];
for (let i = 0; i < 40; i++) objs.push(make(i));
const keys = ["n", "k0", "k1", "k3", "k6", "g", "inherited", "missing", 0, 1, 2, 3, 4, true, false, -0];
function read(o, k) { return o[k]; }
const out = [];
let junk = [];
for (let round = 0; round < 1500; round++) {
    const late = round >= 1450;
    let s = "";
    for (let j = 0; j < objs.length; j += 3) {
        for (let q = 0; q < keys.length; q++) {
            const k = q === 1 && late ? "k" + String(round % 2 === 0 ? 0 : 1) : keys[q];
            const v = read(objs[j], k);
            s += v === undefined ? "u" : typeof v === "string" ? v.length : v;
        }
    }
    if (round === 1455) objs[0].k0 = "w";
    if (round === 1460) delete objs[3].n;
    if (round === 1465) Base.prototype.missing = "now";
    if (round === 1470) Object.defineProperty(Base.prototype, "inherited", { get() { return "G"; } });
    if (round === 1475) objs[6][0] = "zero";
    if (late) { junk = []; for (let z = 0; z < 2000; z++) junk.push({ z: "s" + z }); }
    if (round % 300 === 0 || late) out.push(round + ":" + s.length + ":" + s.slice(0, 60));
}
console.log(out.join("\n"));

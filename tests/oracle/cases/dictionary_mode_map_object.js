// An object used as a map: past Shape::kDictionaryThreshold own properties it
// leaves the transition tree for a hashed table, and nothing a program can see
// changes — reads, writes, `in`, `delete`, enumeration order, JSON, integrity
// levels and accessors all answer the same before and after the crossing.
const N = 5000;

const o = {};
for (let i = 0; i < N; i++) o["k" + i] = i;
let sum = 0;
for (let i = 0; i < N; i++) sum += o["k" + i];
console.log("sum", sum, "keys", Object.keys(o).length);
console.log("first/last", Object.keys(o)[0], Object.keys(o)[N - 1]);
console.log("in", "k0" in o, "k4999" in o, "k5000" in o, o.k5000);

// Overwrite keeps the position.
o.k0 = "zero";
console.log("overwrite", o.k0, Object.keys(o)[0]);

// Delete every other key, then re-add one: it lands at the end.
for (let i = 0; i < N; i += 2) delete o["k" + i];
console.log("after delete", Object.keys(o).length, "k1" in o, "k2" in o, o.k3, o.k2);
o.k2 = "back";
const ks = Object.keys(o);
console.log("re-add", ks[0], ks[ks.length - 1], ks.length);

// for-in sees the same order as Object.keys.
let seen = 0;
let firstIn = null;
for (const k in o) {
    if (firstIn === null) firstIn = k;
    seen++;
}
console.log("for-in", seen, firstIn);

// Symbols and accessors still define on the table.
const s = Symbol("s");
o[s] = "sym";
Object.defineProperty(o, "acc", {
    get() { return 42; },
    enumerable: false,
    configurable: true,
});
console.log("symbol/accessor", o[s], o.acc, Object.getOwnPropertySymbols(o).length,
            Object.keys(o).length, Object.getOwnPropertyNames(o).length);

// JSON round-trips the enumerable string keys.
const back = JSON.parse(JSON.stringify(o));
console.log("json", Object.keys(back).length, back.k1, back.k2, back.acc);

// Integrity levels on the table.
Object.freeze(o);
o.k1 = 99;
o.brandNew = 1;
console.log("frozen", o.k1, o.brandNew, Object.isFrozen(o), delete o.k3, o.k3);

// A prototype past the threshold still serves its children, and a late add
// on it is visible through them.
const proto = {};
for (let i = 0; i < 2000; i++) proto["p" + i] = i;
const child = Object.create(proto);
child.own = 1;
console.log("proto", child.p1999, child.p0, child.own, "p5" in child,
            Object.prototype.hasOwnProperty.call(child, "p5"));
proto.p2000 = "late";
console.log("late", child.p2000);
delete proto.p0;
console.log("proto delete", child.p0, "p0" in child);

// A class instance that crosses the threshold keeps its prototype methods.
class Bag {
    constructor() { this.count = 0; }
    put(k, v) { this[k] = v; this.count++; }
    size() { return this.count; }
}
const bag = new Bag();
for (let i = 0; i < 3000; i++) bag.put("item" + i, i * 2);
console.log("class", bag.size(), bag.item2999, bag instanceof Bag, typeof bag.put,
            Object.keys(bag).length);

// Spread and Object.assign copy the whole table.
const copy = { ...bag };
console.log("spread", Object.keys(copy).length, copy.item1234, copy.count);
const assigned = Object.assign({}, proto);
console.log("assign", Object.keys(assigned).length, assigned.p2000);

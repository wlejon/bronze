// Stable layouts (docs/aot-specialization.md, "S1"): every way an object can
// leave a shape that compiled code reads through, done INSIDE A CALL that
// stands between two reads of the same receiver. The reads before the call
// run warm (static-slot cells published, the shapes watched); the reads and
// writes after it must see what the call did, whatever it was.

class P {
    constructor(a, b) {
        this.a = a;
        this.b = b;
    }
}

function show(v) {
    if (typeof v === "function") return "fn";
    if (typeof v === "string") return JSON.stringify(v);
    return String(v);
}

function probe(o, mutate) {
    const before = o.a + o.b;
    mutate(o);
    const a1 = o.a;
    const b1 = o.b;
    let wrote;
    try {
        o.a = 10;
        o.b = 20;
        wrote = show(o.a) + "/" + show(o.b);
    } catch (e) {
        wrote = e.constructor.name;
    }
    const keys = Object.keys(o);
    return [before, show(a1), show(b1), wrote, keys.length > 6 ? keys.length + " keys" : keys.join(",")].join(" ");
}

function noop(o) {}

// Warm: the same sites, the same shape, many times.
let warm = 0;
for (let i = 0; i < 3000; i++) {
    const p = new P(i, 1);
    warm += p.a + p.b;
    probe(p, noop);
}
console.log("warm", warm);

const leaves = [
    ["add", (o) => { o.c = 3; }],
    ["add-overflow", (o) => { for (let i = 0; i < 40; i++) o["k" + i] = i; }],
    ["delete-read", (o) => { delete o.a; }],
    ["delete-other", (o) => { delete o.b; }],
    ["delete-absent", (o) => { delete o.nothere; }],
    ["readonly", (o) => { Object.defineProperty(o, "a", { writable: false }); }],
    ["non-enumerable", (o) => { Object.defineProperty(o, "b", { enumerable: false }); }],
    ["accessor", (o) => {
        Object.defineProperty(o, "a", {
            get() { return "got"; },
            set(v) { this.shadow = v; },
            configurable: true,
            enumerable: true,
        });
    }],
    ["repr", (o) => { o.a = "str"; o.b = { x: 1 }; }],
    ["set-prototype", (o) => { Object.setPrototypeOf(o, { a: 99, extra: 1 }); }],
    ["set-prototype-null", (o) => { Object.setPrototypeOf(o, null); }],
    ["freeze", (o) => { Object.freeze(o); }],
    ["seal", (o) => { Object.seal(o); }],
    ["prevent-extensions", (o) => { Object.preventExtensions(o); }],
    ["dictionary", (o) => { for (let i = 0; i < 1100; i++) o["m" + i] = i; }],
    ["become-prototype", (o) => { const c = Object.create(o); c.z = 1; }],
    ["become-class-prototype", (o) => { function F() {} F.prototype = o; new F(); }],
    ["sibling", (o) => { const s = new P(5, 6); delete s.a; Object.freeze(s); }],
];

for (const [name, mutate] of leaves) {
    console.log(name, probe(new P(1, 2), mutate));
}

// The same leave, the second time round: the sites have seen a broken layout
// once and must keep answering for fresh objects at the old shape.
for (let i = 0; i < 3; i++) {
    console.log("again", probe(new P(i, i), (o) => { if (i === 1) delete o.a; }));
}

// A leave that happens in a callee two calls deep, between reads in a loop.
function deep(o, n) {
    if (n === 0) {
        delete o.b;
        return;
    }
    deep(o, n - 1);
}
let acc = "";
const q = new P(1, 2);
for (let i = 0; i < 4; i++) {
    acc += q.a + ":" + q.b + " ";
    if (i === 1) deep(q, 3);
}
console.log("deep", acc);

// The getter case reaches the getter, and its setter stores aside.
const g = new P(1, 2);
probe(g, leaves[7][1]);
console.log("accessor shadow", g.shadow);

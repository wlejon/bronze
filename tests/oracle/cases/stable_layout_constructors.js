// Stable layouts (docs/aot-specialization.md, "S1"): compiled constructors
// append along cached edges (transition stores, transition runs, define-adds)
// without the runtime seeing the move. Appends keep every existing slot, so a
// watched shape's facts survive them; these cases build objects whose
// constructors append, branch, read back what they wrote, and call out between
// the writes, then break the layouts afterwards and read again.

class Vec {
    constructor(x, y) {
        this.x = x;
        this.y = y;
        this.len = Math.sqrt(this.x * this.x + this.y * this.y);
    }
}

class Vec3 extends Vec {
    constructor(x, y, z) {
        super(x, y);
        this.z = z;
        this.len = Math.sqrt(this.len * this.len + z * z);
    }
}

function touch(o) {
    o.tag = "t";
}

class Node {
    constructor(v, extra) {
        this.v = v;
        if (extra) this.extra = extra;
        touch(this);
        this.after = this.v + (this.extra ? this.extra : 0);
    }
}

let s = 0;
for (let i = 0; i < 2000; i++) {
    const v = i % 3 === 0 ? new Vec3(i, 1, 2) : new Vec(i, 1);
    s += v.x + v.y + v.len;
    const n = new Node(i, i % 2);
    s += n.after;
}
console.log("built", s.toFixed(3));

// A setter appears on the prototype of a constructed class mid-run: the next
// construction must reach it instead of appending.
Object.defineProperty(Vec.prototype, "y", {
    set(v) { this.yy = v * 10; },
    get() { return this.yy; },
    configurable: true,
});
const w = new Vec(3, 4);
console.log("setter", w.x, w.y, w.yy, Object.keys(w).join(","), w.len.toFixed(3));

// Freeze a constructed object, then build and read more at its old shape.
const f = new Vec3(1, 2, 2);
Object.freeze(f);
const f2 = new Vec3(2, 2, 1);
f2.x = 5;
console.log("frozen", f.x, f.z, f2.x, f2.z, Object.isFrozen(f), Object.isFrozen(f2));

// A constructor whose callee deletes one of the fields already written.
class Shaky {
    constructor(k) {
        this.a = k;
        this.b = k + 1;
        if (k === 7) delete this.a;
        this.c = (this.a === undefined ? 0 : this.a) + this.b;
    }
}
let out = [];
for (let k = 5; k < 10; k++) {
    const o = new Shaky(k);
    out.push(o.a + "/" + o.b + "/" + o.c + "/" + Object.keys(o).join(""));
}
console.log("shaky", out.join(" "));

// A double field that turns into a string inside a call between reads.
class Acc {
    constructor() {
        this.total = 0.5;
        this.count = 0;
    }
}
function spoil(a) {
    a.total = "spoiled";
}
const accs = [];
for (let i = 0; i < 500; i++) {
    const a = new Acc();
    a.total += i * 0.25;
    a.count++;
    accs.push(a);
}
const victim = accs[10];
const t0 = victim.total;
spoil(victim);
const t1 = victim.total;
victim.count += 1;
console.log("repr", t0, t1, victim.count, accs[11].total, new Acc().total + 1);

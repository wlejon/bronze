// A JS value's bits are read around calls that may collect: several times
// between two allocating calls, through loops whose back edges poll,
// through cold paths that call while the hot path does not, and through
// frames an exception unwinds. brass reads the bits from the value's own
// register or slot (no copy of them), and keeps the value where the stack
// map finds it at each such call. Run under BRONZE_GC_STRESS, a read that
// saw a moved object's old address shows in the sums.

class Vec { constructor(x, y, z) { this.x = x; this.y = y; this.z = z; } }

function between(o, n) {
    // Reads of `o` before, between and after allocating calls.
    let s = o.x + o.y + o.z;
    const a = new Vec(o.x, o.y, n);
    s += o.x * 2 + o.y * 3 + o.z * 4;
    const b = [o.x, o.y, o.z, a.z];
    s += o.z - o.x + b.length;
    const c = { p: o, q: a, r: b };
    s += c.p.x + c.q.y + c.r[3] + o.y + o.z;
    return s;
}

function coldCalls(o, k) {
    // The hot path reads `o` and `k` with no call; the cold one allocates.
    let s = 0;
    for (let i = 0; i < 40; i++) {
        s += o.x + o.y;
        if ((i & 15) === k) {
            const t = new Vec(i, o.z, s & 7);
            s += t.x + t.y;
        }
        s += o.z * i;
    }
    return s;
}

function keep(objs) {
    // Several references live through a loop of allocations.
    const a = objs[0], b = objs[1], c = objs[2], d = objs[3], e = objs[4];
    let s = 0;
    for (let i = 0; i < 25; i++) {
        const tmp = [a.x, b.y, c.z, d.x, e.y, i];
        s += tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5];
        s += a.y + b.z + c.x + d.y + e.z;
    }
    return s;
}

function thrower(o, n) {
    if (n % 7 === 3) throw new Error('t' + o.x);
    return new Vec(o.y, o.z, n);
}

function through(o, n) {
    let s = o.x;
    try {
        const v = thrower(o, n);
        s += v.x + v.y + o.z;
    } catch (err) {
        s += err.message.length + o.y + o.z;
    }
    s += o.x + o.y;
    return s;
}

let total = 0;
const objs = [];
for (let i = 0; i < 64; i++) objs.push(new Vec(i, i * 2 + 1, i % 5));
for (let r = 0; r < 200; r++) {
    const o = objs[r & 63];
    total += between(o, r);
    total += coldCalls(o, r & 15);
    total += keep([objs[r & 63], objs[(r + 1) & 63], objs[(r + 2) & 63], objs[(r + 3) & 63], o]);
    total += through(o, r);
    if (r % 50 === 0) console.log('r', r, total);
}
console.log('total', total);

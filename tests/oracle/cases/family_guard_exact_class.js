// Exact-class receivers (a `new C()` result, `this` of a class nobody extends)
// whose sites share one family guard per receiver: a stretch of reads and
// writes on one object asks its shape once. Everything that moves the shape
// between two of those sites must still be seen: a property added or deleted,
// a field made non-writable or an accessor, a prototype set, a getter that
// mutates the receiver, aliased receivers, and a collection in between.

class Quat {
    constructor(x, y, z, w) {
        this._x = x;
        this._y = y;
        this._z = z;
        this._w = w;
    }
    set(x, y, z, w) {
        this._x = x;
        this._y = y;
        this._z = z;
        this._w = w;
        return this;
    }
    mul(a, b) {
        const ax = a._x, ay = a._y, az = a._z, aw = a._w;
        const bx = b._x, by = b._y, bz = b._z, bw = b._w;
        this._x = ax * bw + aw * bx + ay * bz - az * by;
        this._y = ay * bw + aw * by + az * bx - ax * bz;
        this._z = az * bw + aw * bz + ax * by - ay * bx;
        this._w = aw * bw - ax * bx - ay * by - az * bz;
        return this;
    }
    // Reads around a call that may change the receiver's shape.
    sumAround(f) {
        const x = this._x;
        const y = this._y;
        f(this);
        const z = this._z;
        const w = this._w;
        return x + y + z + w;
    }
    // Writes around a call.
    setAround(f, v) {
        this._x = v;
        this._y = v + 1;
        f(this);
        this._z = v + 2;
        this._w = v + 3;
        return this;
    }
    str() {
        return [this._x, this._y, this._z, this._w].join(",");
    }
}

function show(label, q) {
    console.log(label, q.str(), Object.keys(q).join("|"));
}

const results = [];
for (let i = 0; i < 2000; i++) {
    const q = new Quat(1, 2, 3, 4);
    const r = new Quat(0.5, 0.25, 0.125, 1);
    q.mul(q, r);
    if (i === 1999) results.push(q.str());
}
console.log("warm", results.join(";"));

// A property added between reads.
{
    const q = new Quat(1, 2, 3, 4);
    console.log("add", q.sumAround((o) => { o.extra = 9; o._z = 30; }));
    show("add-after", q);
}

// A property deleted between reads: `_z` is gone, so it reads undefined.
{
    const q = new Quat(1, 2, 3, 4);
    console.log("delete", q.sumAround((o) => { delete o._z; }));
    show("delete-after", q);
}

// A field turned into an accessor between reads.
{
    const q = new Quat(1, 2, 3, 4);
    let hits = 0;
    console.log("accessor", q.sumAround((o) => {
        Object.defineProperty(o, "_w", { get() { hits++; return 100; }, configurable: true });
    }));
    console.log("accessor-hits", hits);
}

// A field made non-writable between writes: class code is strict, so the
// store throws and the fields after it keep their values.
{
    const q = new Quat(1, 2, 3, 4);
    try {
        q.setAround((o) => { Object.defineProperty(o, "_z", { writable: false }); }, 10);
        console.log("frozen-field no throw");
    } catch (e) {
        console.log("frozen-field", e instanceof TypeError);
    }
    show("frozen-field-after", q);
}

// The whole object frozen between writes.
{
    const q = new Quat(1, 2, 3, 4);
    try {
        q.setAround((o) => { Object.freeze(o); }, 20);
    } catch (e) {
        console.log("freeze", e instanceof TypeError);
    }
    show("freeze-after", q);
}

// A prototype set between writes, onto one with a setter for `_z`.
{
    const q = new Quat(1, 2, 3, 4);
    const log = [];
    const proto = Object.create(Quat.prototype, {
        _q: { value: 1, writable: true },
    });
    q.setAround((o) => {
        delete o._z;
        Object.setPrototypeOf(o, Object.create(proto, {
            _z: { set(v) { log.push("set " + v); }, get() { return -1; }, configurable: true },
        }));
    }, 30);
    console.log("proto", log.join(","), q._z, q.str());
}

// A getter on the argument that mutates the receiver in the middle of mul's
// reads: `a` and `this` alias.
{
    const q = new Quat(1, 2, 3, 4);
    const b = new Quat(0, 0, 0, 1);
    Object.defineProperty(b, "_y", {
        get() {
            q.extra2 = 1;
            delete q._w;
            return 0;
        },
        configurable: true,
    });
    q.mul(q, b);
    show("getter-mutates", q);
}

// Aliasing: the receiver and both arguments are one object.
{
    const q = new Quat(0.5, 0.5, 0.5, 0.5);
    q.mul(q, q);
    show("alias", q);
}

// Another class with the same field names arrives at the same sites.
{
    class Other {
        constructor() {
            this._w = 4;
            this._x = 1;
            this._y = 2;
            this._z = 3;
        }
    }
    const q = new Quat(0, 0, 0, 1);
    q.mul(new Other(), new Quat(0, 0, 0, 1));
    show("other-order", q);
    const plain = { _x: 1, _y: 2, _z: 3, _w: 4 };
    Quat.prototype.set.call(plain, 5, 6, 7, 8);
    console.log("plain", plain._x, plain._y, plain._z, plain._w);
}

// A collection between reads: the receiver moves, its shape does not.
{
    const q = new Quat(1, 2, 3, 4);
    console.log("gc", q.sumAround(() => {
        let keep = [];
        for (let i = 0; i < 20000; i++) keep.push({ i, s: "x" + i });
        keep = null;
    }));
    q.setAround(() => {
        const junk = [];
        for (let i = 0; i < 20000; i++) junk.push([i, i + 1]);
    }, 40);
    show("gc-after", q);
}

// Fields added after construction, then the exact-class sites again.
{
    const q = new Quat(1, 2, 3, 4);
    for (let i = 0; i < 20; i++) q["late" + i] = i;
    q.set(9, 8, 7, 6);
    q.mul(q, new Quat(0, 0, 0, 1));
    show("late-fields", q);
}

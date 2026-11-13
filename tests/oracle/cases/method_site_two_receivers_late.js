// A method call site that meets a third receiver a few times while starting
// up and two receivers from then on: its two callees keep answering, each for
// its own receivers, and a receiver of neither still gets its own method.
class Quat {
    constructor() { this._x = 0; this._y = 0; this._z = 0; this._w = 1; }
    fromArray(a, o) { this._x = a[o]; this._y = a[o + 1]; this._z = a[o + 2]; this._w = a[o + 3]; return this; }
    sum() { return this._x + this._y + this._z + this._w; }
}
class Vec {
    constructor() { this.x = 0; this.y = 0; this.z = 0; }
    fromArray(a, o) { this.x = a[o]; this.y = a[o + 1]; this.z = a[o + 2]; return this; }
    sum() { return this.x + this.y + this.z; }
}
class Scalar {
    constructor() { this.v = 0; }
    fromArray(a, o) { this.v = a[o] * 2; return this; }
    sum() { return this.v; }
}
const buffer = new Float64Array(64);
for (let i = 0; i < buffer.length; i++) buffer[i] = i * 0.5 + 1;
function apply(target, offset) {
    return target.fromArray(buffer, offset).sum();
}
const q = new Quat(), v = new Vec(), s = new Scalar();
let total = 0;
// The third receiver first, a few times.
for (let i = 0; i < 3; i++) total += apply(s, i);
// Then the two, nine to one.
for (let i = 0; i < 20000; i++) total += i % 10 === 9 ? apply(v, i % 60) : apply(q, i % 60);
console.log(total);
// The third again, late, and an own method on one object.
total += apply(s, 5);
const own = new Vec();
own.fromArray = function (a, o) { this.x = -a[o]; return this; };
total += apply(own, 7);
console.log(total, q.sum(), v.sum(), s.sum(), own.x);
// A site that rotates through five receivers keeps every answer too.
const makers = [() => new Quat(), () => new Vec(), () => new Scalar(), () => ({ fromArray(a, o) { this.k = a[o] + 100; return this; }, sum() { return this.k; } }),
    () => { const o = new Vec(); o.extra = 1; return o; }];
const many = makers.map((m) => m());
let rot = 0;
for (let i = 0; i < 5000; i++) rot += apply(many[i % 5], i % 50);
console.log(rot);

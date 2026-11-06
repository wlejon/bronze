// A base method that only throws (three.js's Interpolant.interpolate_), called
// through `this` from a base method, where only subclasses that override it
// are ever made: the call site must reach each subclass's override, also when
// the instances escape into an array the analysis cannot follow, and a base
// instance made at the end must still reach the throwing base method.
class Base {
    constructor(k) { this.k = k; this.calls = 0; }
    evaluate(t) {
        this.calls++;
        return this.interpolate_(t) + this.k;
    }
    interpolate_(t) { throw new Error('abstract interpolate_ called with ' + t); }
}
class Linear extends Base {
    interpolate_(t) { return t * 2; }
}
class Step extends Base {
    interpolate_(t) { return t < 0.5 ? 0 : 1; }
}
class Smooth extends Linear {
    interpolate_(t) { return t * t * (3 - 2 * t); }
}

const pool = [];
function make(i) {
    switch (i % 3) {
        case 0: return new Linear(i);
        case 1: return new Step(i);
        default: return new Smooth(i);
    }
}
for (let i = 0; i < 9; i++) pool.push(make(i));

let sum = 0;
for (let round = 0; round < 400; round++) {
    const t = (round % 10) / 10;
    for (const it of pool) sum += it.evaluate(t);
}
console.log('sum', sum.toFixed(6));
console.log('calls', pool.map((p) => p.calls).join(','));

// An override installed late on one subclass's prototype.
Step.prototype.interpolate_ = function (t) { return -t; };
let late = 0;
for (const it of pool) late += it.evaluate(0.25);
console.log('late', late.toFixed(6));

// A base instance after all: its own method throws.
try {
    new Base(1).evaluate(0.5);
    console.log('no throw');
} catch (e) {
    console.log('threw', e.message);
}

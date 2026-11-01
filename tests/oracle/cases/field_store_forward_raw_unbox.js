// A read of a field after a store to it, in a method no call inference sees
// reaches (it is applied through Reflect under a computed name, as a host
// calls a method of an object handed to it). Inference leaves an unreached
// method's writes out of the field's audit, so `s` is an audited Number and
// its reads are unboxed unchecked, which each read's own validation answers
// for. The redundant-read pass used to forward the stored value into the
// read, unchecked unbox and all: the value `this.s += a` stores is whatever
// the dynamic add made, a String when `a` is one, and the proof verifier
// refused the module. The read must stay a read.

class Counter {
    constructor() { this.s = 0; }
    step(a) { this.s += a; return this.s >= 3; }
    scaled() { return this.s * 2; }
}

const c = new Counter();
const step = Counter.prototype[['st', 'ep'].join('')];
const out = [];
for (const a of [1, 2, 'x', 5]) {
    out.push(String(Reflect.apply(step, c, [a])), String(c.s));
}
console.log(out.join(','), c.scaled());

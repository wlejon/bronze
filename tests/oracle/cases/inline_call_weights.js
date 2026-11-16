// The inliner's call weights (opt/inline driver.cpp): a large callee called
// often from a loop is taken where a cold call of the same size is not, a
// polymorphic method site shares its count over its arms by their callees'
// entries, and a never-run call stays a call. Each must behave as the call:
// guards inside a hot inlined body still leave correctly when the data turns.

class Acc {
    constructor(seed) { this.a = seed; this.b = seed * 2; this.c = 0; this.d = []; }
    // A body well past the small-callee limit.
    big(x, y) {
        let s = this.a + x;
        s = s * 3 + y; s = (s ^ 7) + this.b; s = s - (x & 15); s = s + (y | 1);
        s = s * 1.5 - this.c; s = Math.floor(s) % 100003; s = s + (x * y) % 17;
        if (s > 5000) s -= 4999; else s += 13;
        s = s + this.a * 2 - this.b; s = s + (x << 2) - (y >> 1); s = s % 99991;
        for (let k = 0; k < 3; k++) s = (s * 31 + k + x) % 65521;
        s = s + (this.d.length > 2 ? this.d[1] : 5);
        s = (s * 7 + y) % 32749; s = s + ((x + y) & 255); s = s - ((x - y) | 0);
        s = (s + this.a) % 10007; s = s + ((s & 1) ? 3 : 4); s = s * 2 - 1;
        s = (s + this.b * 3) % 30011; s = s + (x > y ? x - y : y - x);
        this.c = (this.c + s) % 1000003;
        if (this.d.length < 4) this.d.push(s);
        return s;
    }
}

class Square { area(k) { return k * k; } }
class Circle { area(k) { return 3 * k * k + this.r; } constructor() { this.r = 1; } }
class Rare { area(k) { return -k; } }

function hot(acc, n) {
    let t = 0;
    for (let i = 0; i < n; i++) t = (t + acc.big(i, n - i)) % 1000000007;
    return t;
}

function cold(acc, flag) {
    // Never run in a training-sized pass: flag is false there.
    if (flag) return acc.big(1, 2) + acc.big(3, 4);
    return 0;
}

function shapes(list, n) {
    let t = 0;
    for (let i = 0; i < n; i++) t += list[i % list.length].area(i % 7);
    return t;
}

const acc = new Acc(3);
let out = 0;
for (let r = 0; r < 40; r++) out = (out + hot(acc, 200)) % 1000000007;
console.log("hot", out, acc.c);
console.log("cold", cold(acc, false), cold(acc, out !== 0.5));

const list = [];
for (let i = 0; i < 50; i++) list.push(i % 25 === 0 ? new Circle() : new Square());
let s = 0;
for (let r = 0; r < 100; r++) s += shapes(list, 300);
console.log("shapes", s);
// The rare arm and a receiver of a new class only now.
list.push(new Rare());
console.log("shapes+rare", shapes(list, 300));

// The hot callee's data turns: a string where numbers ran, then a getter.
const acc2 = new Acc(5);
console.log("hot2", hot(acc2, 50));
acc2.a = "7";
console.log("turned", hot(acc2, 20));
Object.defineProperty(acc2, "b", { get() { return 11; } });
console.log("getter", hot(acc2, 20));

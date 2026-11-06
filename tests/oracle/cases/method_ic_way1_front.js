// A method site of two receiver shapes keeps both in its two ways, and a hit
// in way 1 moves that entry to way 0 (the method stub's move to front). Every
// answer must stay the receiver's own method whichever way holds it: runs of
// one shape and alternation, a prototype method replaced after the entries
// traded places, an own property shadowing the inherited one, a third shape,
// a SLOT entry in way 0 (never moved to way 1), and methods closing over
// environments made fresh, with collections in between, so the env words
// trade places while the environments are young and after they are old.
class Base { constructor(k) { this.k = k; } m(a) { return "base:" + this.k + ":" + a; } }
class P extends Base { constructor() { super("P"); this.p = 1; } m(a) { return "P" + (a + this.p); } }
class Q extends Base { constructor() { super("Q"); this.q = 2; this.qq = 3; } m(a) { return "Q" + (a * this.q); } }
class R extends Base { constructor() { super("R"); this.r = 4; } }

function call(o, a) { return o.m(a); }

function run(label, objs, reps) {
    let h = 0;
    let last = "";
    for (let r = 0; r < reps; r++) {
        for (const o of objs) {
            last = call(o, r);
            for (let i = 0; i < last.length; i++) h = (h * 31 + last.charCodeAt(i)) | 0;
        }
    }
    console.log(label, h, last);
}

const p = new P(), q = new Q(), r = new R();
// Q latches first, then P displaces it into way 1.
run("latch", [q, p], 1);
// A run of Q's (way-1 hits; the first moves Q to front), then of P's.
run("runQ", [q, q, q, q, q, q, q, q], 50);
run("runP", [p, p, p, p, p, p, p, p], 50);
// Alternation: every call a way-1 hit.
run("alt", [p, q], 200);
run("skew", [q, q, q, q, q, p], 200);

// A prototype method replaced while either way may hold it.
Q.prototype.m = function (a) { return "Q2:" + (a - this.qq); };
run("patchQ", [q, p, q, q], 50);
P.prototype.m = function (a) { return "P2:" + (a + 100); };
run("patchP", [p, q, p, p], 50);

// An own property shadowing the inherited one on one instance.
const q2 = new Q();
q2.m = function (a) { return "own:" + a; };
run("own", [q, q2, p, q2], 50);

// A third shape rotating with the two.
run("three", [p, q, r, q, p], 50);

// A receiver whose method is its own slot latches the SLOT form in way 0;
// a way-1 hit after it must leave it there and still answer.
class S { constructor(n) { this.n = n; this.m = (a) => "S" + (a + this.n); } }
const s1 = new S(1), s2 = new S(2);
run("slot", [p, s1, q, s2, p, s1, q], 50);

// Methods closing over a fresh environment per class, so the env words of
// the site's entries are young objects when they latch; garbage between the
// rounds moves them to the old generation while the entries trade places.
function make(tag) {
    const salt = [tag, tag.length];
    return class { constructor() { this.t = tag; } m(a) { return salt[0] + ":" + (a + salt[1]); } };
}
const keep = [];
for (let round = 0; round < 12; round++) {
    const A = make("a" + round), B = make("bb" + round);
    const a = new A(), b = new B();
    run("env" + round, [a, b, b, b, a, b], 20);
    let junk = [];
    for (let i = 0; i < 20000; i++) junk.push({ i, s: "x" + i });
    keep.push(junk.length);
    run("envafter" + round, [b, a, a, b], 20);
}
console.log("kept", keep.length);

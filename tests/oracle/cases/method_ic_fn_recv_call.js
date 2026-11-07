// A method call whose receiver names a function binding (il.h `icFnRecv`),
// `Quaternion.slerpFlat(...)`: the site tests the method cache's FUNCTION
// receiver form itself — way 0 the SLOT form keyed on the shape of the
// function's statics box — and enters the callee read out of the box, where
// before it called the method stub, which reached that arm last. Every call
// below runs through a warm site and must still be the function the box holds
// NOW, with the class as `this`: a static reassigned to another function or
// to a non-function, a static added (the box's shape moves), deleted, turned
// into an accessor, an inherited static (never latched), two classes whose
// boxes share a shape behind one binding, an ordinary function's own
// property, an intrinsic constructor's static, arguments past and short of
// the callee's arity, and collections between latch and hit (a static closure
// over a young environment).

function say(label, value) { console.log(label + "=" + value); }

class Q {
    static slerp(dst, a, b, t) {
        for (let i = 0; i < 4; i++) dst[i] = a[i] + (b[i] - a[i]) * t;
        return dst;
    }
    static who() { return this.name; }
    static add(a, b, c) { return a + b + (c === undefined ? 100 : c); }
}

function lerpAll(n) {
    const d = [0, 0, 0, 0], a = [1, 2, 3, 4], b = [5, 6, 7, 8];
    let s = 0;
    for (let i = 0; i < n; i++) { Q.slerp(d, a, b, i / n); s += d[0] + d[3]; }
    return s;
}
say("warm.slerp", lerpAll(200).toFixed(3));

function who() { return Q.who(); }
let w = "";
for (let i = 0; i < 50; i++) w = who();
say("warm.this", w);

// Arity: fewer arguments leave the rest undefined, more are dropped.
function adds() {
    let s = 0;
    for (let i = 0; i < 40; i++) s += Q.add(i, 1) + Q.add(i, 1, 2, 3);
    return s;
}
say("arity", adds());

// Reassigned to another function: same box shape, same slot, new callee.
const oldSlerp = Q.slerp;
Q.slerp = function (dst, a, b, t) { dst[0] = -t; dst[3] = this === Q ? 1 : 0; return dst; };
say("reassigned", lerpAll(100).toFixed(3));
Q.slerp = oldSlerp;
say("restored", lerpAll(100).toFixed(3));

// A static that is no function: the site's own test refuses it, the helper throws.
const keep = Q.who;
Q.who = 42;
try { who(); say("nonfn", "no throw"); } catch (e) { say("nonfn", e instanceof TypeError); }
Q.who = keep;
say("nonfn.restored", who());

// Added: the box moves to a new shape; the site relatches.
Q.extra = 1;
for (let i = 0; i < 30; i++) w = who();
say("added", w + lerpAll(10).toFixed(2));

// Deleted: the box changes shape (or leaves the plain form); answers stay right.
delete Q.extra;
for (let i = 0; i < 30; i++) w = who();
say("deleted", w);
delete Q.add;
try { adds(); say("deleted.add", "no throw"); } catch (e) { say("deleted.add", e instanceof TypeError); }

// An accessor installed over a warm static.
Object.defineProperty(Q, "who", { get() { return () => "getter-" + this.name; }, configurable: true });
for (let i = 0; i < 30; i++) w = who();
say("accessor", w);

// Inherited statics are not latched (depth 1); `this` is the subclass.
class R extends Q { static own() { return "R-own"; } }
function inherited() { return R.slerp([0, 0, 0, 0], [1, 1, 1, 1], [3, 3, 3, 3], 0.5)[2] + ":" + R.own(); }
let inh = "";
for (let i = 0; i < 30; i++) inh = inherited();
say("inherited", inh);

// Two classes whose boxes share a shape, called through one binding.
class A1 { static make(x) { return "A1:" + x + ":" + this.name; } }
class B1 { static make(x) { return "B1:" + x + ":" + this.name; } }
let K = A1;
function make(x) { return K.make(x); }
let m = "";
for (let i = 0; i < 40; i++) { if (i === 20) K = B1; m += make(i).charAt(0); }
say("shared.shape", m + " " + make(7));

// An ordinary function's own property.
function F() {}
F.helper = function (a) { return a * 2 + (this === F ? 1 : 0); };
function viaF() { let s = 0; for (let i = 0; i < 40; i++) s += F.helper(i); return s; }
say("function.prop", viaF());
F.helper = (a) => a;
say("function.prop2", viaF());

// An intrinsic constructor's static (the EXOTIC form, never the box arm).
function keys() { let s = 0; for (let i = 0; i < 30; i++) s += Object.keys({ a: i, b: 2 }).length; return s; }
say("intrinsic", keys());

// Collections between the latch and the hits: a static closure over a young
// environment, replaced while garbage piles up.
class G { static step(x) { return x + 1; } }
function churn(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        const junk = [];
        for (let j = 0; j < 20; j++) junk.push({ j, k: [j, j + 1] });
        if (i % 50 === 25) { const base = i; G.step = (x) => x + base + junk.length; }
        s = (s + G.step(i)) % 1000003;
    }
    return s;
}
say("gc", churn(400));

// A method call whose receivers hold the callee in an OWN slot under a key
// no class declares as a method (three.js's PropertyMixer
// `this._mixBufferRegion = this._slerp`, WebGLState's object of closures):
// the method cache latches the SLOT form, and the site's out-of-line test
// enters the callee re-read from the receiver's slot instead of calling the
// method stub. Every answer must be the function the receiver holds NOW,
// with the receiver as `this`: same-shape receivers holding different
// closures, a slot reassigned to another function or to a non-function, a
// receiver of another shape at the warm site, the slot's key turned into an
// accessor, arguments past and short of the callee's arity, and collections
// between the latch and the hits (closures over young environments).

function say(label, value) { console.log(label + "=" + value); }

class Mixer {
    constructor(kind) {
        this.n = 0;
        this.buf = [1, 2, 3, 4];
        this._mix = kind === "slerp" ? this._slerp : this._lerp;
    }
    _slerp(i, w) { this.n += 2; return this.buf[i & 3] * w + 0.5; }
    _lerp(i, w) { this.n += 1; return this.buf[i & 3] * w; }
    accumulate(i, w) { return this._mix(i, w); }
}

function run(ms, reps) {
    let s = 0;
    for (let r = 0; r < reps; r++) for (const m of ms) s += m.accumulate(r, 0.25);
    return s;
}

const a = new Mixer("slerp"), b = new Mixer("lerp"), c = new Mixer("slerp");
say("warm", run([a, b, c], 50) + " " + a.n + " " + b.n + " " + c.n);

// Reassigned to another function: same shape, same slot.
a._mix = function (i, w) { return this === a ? 1000 : -1; };
say("reassigned", run([a, b], 10));

// A non-function in the slot: the helper's TypeError.
b._mix = 7;
try { run([b], 3); say("nonfn", "no throw"); } catch (e) { say("nonfn", e instanceof TypeError); }
b._mix = b._lerp;
say("nonfn.restored", run([b], 4));

// An object literal's closures, called through one site.
function makeState(tag) {
    let calls = 0;
    return {
        enable(x) { calls++; return tag + ":" + x + ":" + calls; },
        disable(x) { calls--; return tag + "-" + x; },
    };
}
function useState(st, k) { return st.enable(k); }
const s1 = makeState("A"), s2 = makeState("B");
let out = "";
for (let i = 0; i < 30; i++) out = useState(i < 15 ? s1 : s2, i);
say("literal", out + " " + useState(s1, 99));

// Another shape at the warm site, then back.
const odd = { _mix(i, w) { return 42; }, buf: [], n: 0, accumulate: Mixer.prototype.accumulate };
say("othershape", run([odd, c], 5));

// The key made an accessor on one receiver: the shape moves, the getter's function is called.
Object.defineProperty(c, "_mix", { get() { return (i, w) => -i; }, configurable: true });
say("accessor", run([c, a], 3));

// Arity.
class Arity { constructor() { this.f = (x, y, z) => (x | 0) + (y === undefined ? 10 : y) + (z === undefined ? 100 : z); } call2(i) { return this.f(i, 1); } call4(i) { return this.f(i, 1, 2, 3); } }
const ar = new Arity();
let as = 0;
for (let i = 0; i < 40; i++) as += ar.call2(i) + ar.call4(i);
say("arity", as);

// Collections between latch and hits: closures over young environments.
class Holder { constructor(k) { this.k = k; this.fn = (x) => x + k; } go(x) { return this.fn(x); } }
let g = 0;
const hs = [];
for (let i = 0; i < 300; i++) {
    const junk = [];
    for (let j = 0; j < 30; j++) junk.push({ j, v: [j, i] });
    if (i % 40 === 0) hs.push(new Holder(i + junk.length));
    for (const h of hs) g = (g + h.go(i)) % 1000003;
    if (i % 75 === 74) hs[0].fn = (x) => x * 2;
}
say("gc", g);

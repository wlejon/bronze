// Property sites under shape churn, read and written through their way 0.
// Generated code's way-0 guards compare a site's packed row (bronze_abi_layout.h,
// "the HOT array"), a copy of way 0 the runtime refreshes on every fill and the
// move-to-front read stub refreshes on its own install; the ways behind it and
// the epoch stay in the site. Every answer below must be the receiver's own as
// the sites refill: one constructor's shape then another's, a property added
// after a site warmed, deletes that make a dictionary and re-adds after it,
// more shapes than the site has ways, sets that transition and sets into f64
// slots that a non-number generalizes, and collections between the fills.
class P { constructor(a, b) { this.a = a; this.b = b; } }
class Q { constructor(a, b) { this.b = b; this.a = a; this.c = 0; } }

function readA(o) { return o.a; }
function readB(o) { return o.b; }
function writeA(o, v) { o.a = v; }
function addLate(o, v) { o.late = v; return o.late; }

const out = [];
function note(label, v) { out.push(label + "=" + String(v)); }

// One shape, then another at the same sites, alternating.
let h = 0;
const ps = [], qs = [];
for (let i = 0; i < 16; i++) { ps.push(new P(i, i * 2)); qs.push(new Q(i * 3, i * 4)); }
for (let r = 0; r < 50; r++) {
    const xs = r % 3 === 0 ? qs : ps;
    for (const o of xs) h = (h * 31 + readA(o) + 7 * readB(o)) | 0;
}
note("alternate", h);

// More shapes than ways: literals with a different first key each.
const many = [];
for (let k = 0; k < 9; k++) {
    const o = {};
    o["k" + k] = k;
    o.a = k * 10;
    o.b = k * 100;
    many.push(o);
}
h = 0;
for (let r = 0; r < 40; r++) {
    for (let i = 0; i < many.length; i++) {
        const o = many[(i * 5 + r) % many.length];
        h = (h * 33 + readA(o) - readB(o)) | 0;
    }
}
note("many", h);

// Writes through way 0, a site that transitions, and reads of the result.
for (let r = 0; r < 30; r++) {
    for (const o of ps) writeA(o, readA(o) + r);
    for (const o of qs) writeA(o, readB(o) - r);
}
h = 0;
for (const o of ps) h = (h * 17 + o.a) | 0;
for (const o of qs) h = (h * 19 + o.a) | 0;
note("written", h);
h = 0;
for (let i = 0; i < ps.length; i++) h = (h * 13 + addLate(ps[i], i) + addLate(qs[i], -i)) | 0;
note("late", h);
h = 0;
for (let r = 0; r < 20; r++) for (const o of ps) h = (h * 7 + readA(o) + (o.late | 0)) | 0;
note("after-late", h);

// Deletes: the site's shape goes to a dictionary and comes back as a new one.
const d = new P(1, 2);
let dh = 0;
for (let r = 0; r < 40; r++) {
    dh = (dh * 3 + readA(d) + readB(d)) | 0;
    if (r === 10) delete d.b;
    if (r === 20) d.b = 5;
    if (r === 30) { delete d.a; d.a = 9; }
}
note("deletes", dh);
note("readB-missing", readB({ a: 1 }));

// Doubles in f64 slots, then a string written over one.
class V { constructor(x) { this.x = x; this.y = x / 2; } }
function readX(o) { return o.x; }
function writeX(o, v) { o.x = v; }
const vs = [];
for (let i = 0; i < 8; i++) vs.push(new V(i + 0.5));
let s = 0;
for (let r = 0; r < 30; r++) for (const o of vs) { writeX(o, readX(o) * 1.25); s += readX(o); }
note("doubles", s.toFixed(6));
writeX(vs[3], "str");
let t = "";
for (const o of vs) t += typeof readX(o)[0] === "undefined" ? "n" : readX(o);
note("generalized", t);
for (const o of vs) writeX(o, 1.5);
s = 0;
for (const o of vs) s += readX(o);
note("redoubled", s);

// Collections between fills: fresh objects each round, old ones dropped.
let g = 0;
for (let r = 0; r < 200; r++) {
    const fresh = r % 2 ? new P(r, -r) : new Q(-r, r);
    const junk = [];
    for (let j = 0; j < 50; j++) junk.push({ a: j, b: [j, r] });
    g = (g * 29 + readA(fresh) + readB(fresh) + junk[r % 50].b[1]) | 0;
}
note("gc", g);

console.log(out.join("\n"));

// A claimed property site whose first receiver was an object still being
// built: its identity cell names the shape before the last keys were added,
// and a receiver whose shape grew from it, one or two keys on, publishes the
// cell again. Every read and write must answer for the receiver it has, the
// older shape after the move, a sibling holding the key at another slot, a
// receiver of no relation, a deleted key, a non-object, and a getter that
// replaces the data property on a grown receiver.
function readU(p) { return p.uniforms; }
function writeU(p, v) { p.uniforms = v; }
function readU2(p) { return p.uniforms; }

function record(n, grow) {
    const r = {};
    r.version = n;
    r.uniforms = { n };
    if (grow >= 1) r.receiveShadow = n & 1;
    if (grow >= 2) r.lights = n * 2;
    return r;
}

let h = 0;
function mix(v) { h = (h * 31 + (typeof v === "object" && v !== null ? v.n | 0 : String(v).length)) | 0; }

// The first read of each site meets the record before its last key.
const early = record(-1, 0);
mix(readU(early));
writeU(early, { n: -2 });
mix(readU(early));

// Then the grown records, one key on, after it at the same sites (a call
// inlined once per caller is a site per caller).
const grown = [];
for (let i = 0; i < 64; i++) grown.push(record(i, 1));
const all = [early, ...grown];
for (let r = 0; r < 200; r++) {
    for (const g of all) {
        mix(readU(g));
        if ((r & 7) === 0) writeU(g, { n: r + g.version });
        else if (g === early && r === 101) writeU(g, { n: -5 });
    }
}
console.log("grown", h, readU(grown[5]).n);

// The older shape after the cell moved: still answered.
mix(readU(early));
writeU(early, { n: 77 });
console.log("early", readU(early).n, h);

// Two keys on, from a fresh early record's site state.
const twice = [record(-3, 0)];
for (let i = 0; i < 32; i++) twice.push(record(i, 2));
for (let r = 0; r < 100; r++) for (const t of twice) mix(readU2(t));
console.log("twice", h, readU2(twice[3]).n, readU2(early).n);

// The key at another slot, a receiver of no relation, a deleted key,
// non-objects.
const sibling = { uniforms: { n: 500 }, version: 1, receiveShadow: 0 };
const other = { a: 1, b: 2, uniforms: { n: 600 } };
const gone = record(9, 1);
delete gone.uniforms;
for (let r = 0; r < 20; r++) {
    mix(readU(sibling)); mix(readU(other)); mix(readU(gone)); mix(readU(grown[r & 63]));
    writeU(sibling, { n: 500 + r }); writeU(other, { n: 600 + r });
}
console.log("mixed", h, readU(sibling).n, readU(other).n, readU(gone));
let caught = 0;
try { readU(null); } catch (e) { caught = e instanceof TypeError ? 1 : 2; }
console.log("prims", readU("str"), readU(42), caught);

// A getter where the data property was, on a grown record.
const odd = record(3, 1);
Object.defineProperty(odd, "uniforms", { get() { return { n: 999 }; }, set(v) { this.setTo = v.n; }, configurable: true });
writeU(odd, { n: 5 });
console.log("accessor", readU(odd).n, odd.setTo, readU(grown[1]).n);

// A property write site holds one way; a receiver of another shape is
// answered by the thread's megamorphic table, and a site that keeps missing
// its way is relatched by the helper to the shape it now meets. Writes here
// go through sites whose way 0 goes stale (a shape seen once at the start,
// then never again), sites that churn between many shapes, sites whose
// receivers turn into dictionaries, and setters and frozen objects on the
// prototype chain, so a relatch that wrote the wrong slot or skipped a
// setter would show in the hashes.
function mk(i) {
    const o = {};
    for (let k = 0; k < (i % 6); k++) o["k" + k] = k;
    o.x = 0; o.y = 0;
    return o;
}
function setX(o, v) { o.x = v; }
function setXY(o, v) { o.x = v; o.y = v * 2; }
function sum(objs) { let s = 0; for (const o of objs) s = (s * 31 + o.x + o.y * 7) | 0; return s; }

// Stale way 0: one shape first, then a run of others for long enough to
// relatch, then the first shape again.
const first = { x: 0, y: 0, lone: 1 };
setX(first, 1);
const churn = [];
for (let i = 0; i < 12; i++) churn.push(mk(i));
for (let r = 0; r < 400; r++) for (let i = 0; i < churn.length; i++) setX(churn[i], r + i);
setX(first, 99);
console.log("stale", first.x, sum(churn));

// Many shapes at two sites in one function.
for (let r = 0; r < 300; r++) for (const o of churn) setXY(o, r ^ o.x);
console.log("churn", sum(churn));

// Receivers that become dictionaries mid-run.
const dicts = [];
for (let i = 0; i < 8; i++) { const o = mk(i); o.tmp = i; delete o.tmp; dicts.push(o); }
for (let r = 0; r < 200; r++) for (const o of dicts) setX(o, r * 3 + 1);
console.log("dicts", sum(dicts));

// A setter on the prototype of some receivers, a frozen object, a
// non-writable inherited x, and a new property added by the write.
let setterHits = 0;
const withSetter = Object.create({ set x(v) { setterHits += v; } });
const frozen = Object.freeze({ x: 5, y: 0 });
const readOnlyProto = Object.create(Object.defineProperty({}, "x", { value: 9, writable: false }));
const fresh = [];
for (let i = 0; i < 6; i++) fresh.push({ y: i });
const mixed = [...churn.slice(0, 5), withSetter, frozen, readOnlyProto, ...fresh];
for (let r = 0; r < 200; r++) for (const o of mixed) setX(o, r);
console.log("mixed", setterHits, frozen.x, readOnlyProto.x, Object.keys(readOnlyProto).length,
            fresh.map((o) => o.x).join(","), sum(churn));

// Under GC pressure: the relatched shape must stay valid across moves.
for (let r = 0; r < 100; r++) {
    for (const o of churn) setXY(o, r + 1);
    const junk = []; for (let j = 0; j < 500; j++) junk.push({ j, k: [j] });
}
console.log("gc", sum(churn));

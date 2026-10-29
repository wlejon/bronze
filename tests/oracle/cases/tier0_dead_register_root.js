// A collection at a call roots only the frame registers whose values are live
// there. The second loop's slow-path copy is laid out after the code that
// follows the loop, so a value of that copy spans those calls in layout order
// without ever being written on the fast path; its register still holds a
// string from the first section, one an earlier collection freed while the
// register was dead. Rooting it at the property read after the loop (whose
// first global lookup builds the realm, allocating) traced the freed string.
// The shape of the program is the reproduction: under GC stress at tier 0 it
// crashed in the collector's marking.
function bump(acc, v) {
}
const acc = { visited: 0, sumX: 0, sumY: 0 };
const frozen = Object.freeze({ visited: 1, sumX: 2, sumY: 3 });
function readPair(o) { return String(o.a) + ':' + String(o.b); }
let pr = '';
for (let i = 0; i < 100; i++) pr = readPair({ a: i, b: i + 1 });
const del = { a: 5, b: 6 };
delete del.a;
const acc2 = { a: 7, b: 8 };
Object.defineProperty(acc2, 'b', { get() { return 'got'; } });
const viaProto = { __proto__: { a: 'inherited' }, b: 9 };
console.log('3', pr, readPair(del), readPair(acc2), readPair(viaProto), Object.keys(viaProto).join(','));
function readXY(o) { return o.x * 10 + o.y; }
let xy = 0;
for (let i = 0; i < 100; i++) xy += readXY({ x: 1, y: 2 });
console.log('4', xy, readXY(globalThis.spread || { x: 3, y: "Y" }));

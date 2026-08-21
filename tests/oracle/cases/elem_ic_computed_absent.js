// The ABSENT half of the computed-read cache (runtime/elem_ic.h): a `o[k]`
// that finds nothing, everywhere, may say so from an entry — and every way
// that answer can stop being true.
//
// `elem_ic_computed_read.js` next door pins the PRESENT half. This one exists
// because absence has a validity condition the present answer does not: a
// present entry is retired by the receiver's shape alone, while an absent one
// is only as good as the whole prototype chain, and the chain is covered by an
// epoch rather than by the shape the entry names. So each scenario below warms
// the pair three hundred times and then makes the key appear somewhere.
//
// Every scenario prints the answer before AND after, because a cache that
// never fills passes the first half of each of them.

// --- 1. the key appears on the RECEIVER (an own add transitions the shape) --
const holder = { present: 1 };
let a1 = 'x';
for (let i = 0; i < 300; i = i + 1) a1 = holder['missing'];
console.log(a1 === undefined);
holder.missing = 5;
console.log(holder['missing']);

// --- 2. the key appears on the immediate PROTOTYPE -------------------------
const proto2 = { anchor: 0 };
const child2 = Object.create(proto2);
child2.own = 1;
let a2 = 'x';
for (let i = 0; i < 300; i = i + 1) a2 = child2['later'];
console.log(a2 === undefined);
proto2.later = 'from-proto';
console.log(child2['later']);

// --- 3. two links up ------------------------------------------------------
const top3 = {};
const mid3 = Object.create(top3);
const leaf3 = Object.create(mid3);
leaf3.k = 1;
for (let i = 0; i < 300; i = i + 1) leaf3['deep'];
console.log(leaf3['deep'] === undefined);
top3.deep = 'grandparent';
console.log(leaf3['deep']);

// --- 4. Object.defineProperty on the receiver ------------------------------
const dp4 = { a: 1 };
for (let i = 0; i < 300; i = i + 1) dp4['later'];
console.log(dp4['later'] === undefined);
Object.defineProperty(dp4, 'later', { value: 42, enumerable: true, configurable: true });
console.log(dp4['later']);

// --- 5. a prototype GETTER appearing ---------------------------------------
// The entry says "nothing anywhere"; what appears is not a value in a slot but
// a call, so a cache that answered from the depth word would read slot 0.
const pg5 = {};
const child5 = Object.create(pg5);
child5.x = 1;
for (let i = 0; i < 300; i = i + 1) child5['g'];
console.log(child5['g'] === undefined);
Object.defineProperty(pg5, 'g', { get: function () { return 'getter'; } });
console.log(child5['g']);

// --- 6. Object.setPrototypeOf ----------------------------------------------
const sp6 = { own: 1 };
for (let i = 0; i < 300; i = i + 1) sp6['fresh'];
console.log(sp6['fresh'] === undefined);
Object.setPrototypeOf(sp6, { fresh: 'swapped' });
console.log(sp6['fresh']);

// --- 7. __proto__ = --------------------------------------------------------
const pp7 = { own: 1 };
for (let i = 0; i < 300; i = i + 1) pp7['viaProto'];
console.log(pp7['viaProto'] === undefined);
pp7.__proto__ = { viaProto: 'dunder' };
console.log(pp7['viaProto']);

// --- 8. a null-prototype receiver ------------------------------------------
const np8 = Object.create(null);
np8.here = 1;
for (let i = 0; i < 300; i = i + 1) np8['nope'];
console.log(np8['nope'] === undefined);
np8.nope = 'added';
console.log(np8['nope']);

// --- 9. `undefined` AS A VALUE is not absence ------------------------------
const uv9 = { u: undefined, other: 1 };
for (let i = 0; i < 300; i = i + 1) uv9['u'];
console.log(uv9['u'] === undefined, 'u' in uv9);
uv9.u = 7;
console.log(uv9['u']);

// --- 10. an INDEX-LIKE key is never cached ---------------------------------
// A String exotic object synthesises exactly these own properties from its
// character data rather than from a shape, so the refusal is about the KEY and
// not about this receiver.
const nk10 = { 5: 'five' };
for (let i = 0; i < 300; i = i + 1) nk10[7];
console.log(nk10[7] === undefined);
nk10[7] = 'seven';
console.log(nk10[7]);

// --- 11. `length` is refused for the same reason ---------------------------
const len11 = { a: 1 };
for (let i = 0; i < 300; i = i + 1) len11['length'];
console.log(len11['length'] === undefined);
len11.length = 3;
console.log(len11['length']);

// --- 12. a BOOLEAN key is a name and gets an entry of its own ---------------
const bk12 = { true: 'yes' };
for (let i = 0; i < 300; i = i + 1) bk12[false];
console.log(bk12[false] === undefined);
bk12[false] = 'no';
console.log(bk12[false], bk12[true]);

// --- 13. a Proxy is never cached, and the trap count is the pin ------------
let traps13 = 0;
const px13 = new Proxy({}, { get: function () { traps13 = traps13 + 1; return undefined; } });
for (let i = 0; i < 50; i = i + 1) px13['anything'];
console.log(traps13);

// --- 14. a dictionary-mode receiver ----------------------------------------
const dict14 = {};
for (let i = 0; i < 60; i = i + 1) dict14['k' + i] = i;
for (let i = 0; i < 60; i = i + 1) delete dict14['k' + i];
for (let i = 0; i < 300; i = i + 1) dict14['gone'];
console.log(dict14['gone'] === undefined);
dict14.gone = 'now';
console.log(dict14['gone']);

// --- 15. one key absent from TWO shapes ------------------------------------
// Two entries, because the bucket is (shape, key) and not key alone; making it
// present on one must not answer for the other.
const s15a = { a: 1 };
const s15b = { b: 2 };
for (let i = 0; i < 300; i = i + 1) { s15a['zz']; s15b['zz']; }
console.log(s15a['zz'] === undefined, s15b['zz'] === undefined);
s15a.zz = 'A';
console.log(s15a['zz'], s15b['zz'] === undefined);

// --- 16. a key built at RUN TIME, present and absent at one site -----------
const rt16 = { alpha: 1 };
let seen16 = 0;
for (let i = 0; i < 300; i = i + 1) {
    if (rt16['al' + 'pha'] === 1) seen16 = seen16 + 1;
    if (rt16['be' + 'ta'] === undefined) seen16 = seen16 + 1;
}
console.log(seen16);

// --- 17. delete, then re-add ----------------------------------------------
const dl17 = { gone: 1, kept: 2 };
console.log(dl17['gone']);
delete dl17.gone;
for (let i = 0; i < 300; i = i + 1) dl17['gone'];
console.log(dl17['gone'] === undefined);
dl17.gone = 'back';
console.log(dl17['gone'], dl17['kept']);

// --- 18. a key that names nothing ANYWHERE, asked of many fresh receivers --
// The budget the arena key table spends is per distinct KEY, not per read, so
// one name asked of a thousand objects costs one copy.
let miss18 = 0;
for (let i = 0; i < 300; i = i + 1) {
    const fresh = { i: i };
    if (fresh['neverHere'] === undefined) miss18 = miss18 + 1;
}
console.log(miss18);

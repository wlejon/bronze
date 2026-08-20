// The NEGATIVE property cache and every way its answer can stop being
// `undefined`.
//
// A read that finds nothing on the receiver and nothing on its prototype chain
// now records "absent" against the receiver's shape and answers inline. That
// entry is wrong the moment the key appears anywhere the walk covered, and the
// only two things that can notice are the receiver's own shape (which every
// own add transitions) and the prototype-mutation epoch (which every add to a
// prototype, every definition, and every chain swap bumps).
//
// Each scenario warms with a loop first, so a stale entry would answer with the
// wrong constant three hundred times rather than once.

// --- 1. the key appears on the RECEIVER ------------------------------------
class A1 {}
const r1 = new A1();
let c1 = 0;
for (let i = 0; i < 300; i = i + 1) c1 = c1 + (r1.marker === undefined ? 1 : 0);
console.log(c1);
r1.marker = 1;
let s1 = 0;
for (let i = 0; i < 300; i = i + 1) s1 = s1 + r1.marker;
console.log(s1);

// --- 2. the key appears on the IMMEDIATE prototype -------------------------
class A2 {}
class B2 extends A2 {}
const r2 = new B2();
let c2 = 0;
for (let i = 0; i < 300; i = i + 1) c2 = c2 + (r2.tag === undefined ? 1 : 0);
console.log(c2);
B2.prototype.tag = 5;
let s2 = 0;
for (let i = 0; i < 300; i = i + 1) s2 = s2 + r2.tag;
console.log(s2);

// --- 3. the key appears TWO links up ---------------------------------------
// The receiver's shape is untouched by this add, which is the whole reason the
// epoch exists.
class A3 {}
class B3 extends A3 {}
class C3 extends B3 {}
const r3 = new C3();
let c3 = 0;
for (let i = 0; i < 300; i = i + 1) c3 = c3 + (r3.deep === undefined ? 1 : 0);
console.log(c3);
A3.prototype.deep = 7;
let s3 = 0;
for (let i = 0; i < 300; i = i + 1) s3 = s3 + r3.deep;
console.log(s3);

// --- 4. the key is DEFINED on a prototype ----------------------------------
// Non-enumerable and non-writable, which an assignment could never have made,
// so this is the definition path rather than the set path.
class A4 {}
class B4 extends A4 {}
const r4 = new B4();
let c4 = 0;
for (let i = 0; i < 300; i = i + 1) c4 = c4 + (r4.hidden === undefined ? 1 : 0);
console.log(c4);
Object.defineProperty(A4.prototype, "hidden", { value: 11, enumerable: false });
let s4 = 0;
for (let i = 0; i < 300; i = i + 1) s4 = s4 + r4.hidden;
console.log(s4);

// --- 5. a GETTER appears on a prototype ------------------------------------
class A5 {}
const r5 = new A5();
let c5 = 0;
for (let i = 0; i < 300; i = i + 1) c5 = c5 + (r5.computed === undefined ? 1 : 0);
console.log(c5);
Object.defineProperty(A5.prototype, "computed", {
    get: function () {
        return 42;
    },
});
console.log(r5.computed);

// --- 6. setPrototypeOf hands the receiver a chain that HAS the key ---------
const donor6 = { swapped: 9 };
const r6 = {};
let c6 = 0;
for (let i = 0; i < 300; i = i + 1) c6 = c6 + (r6.swapped === undefined ? 1 : 0);
console.log(c6);
Object.setPrototypeOf(r6, donor6);
console.log(r6.swapped);

// --- 7. `__proto__`, the other spelling of the same swap -------------------
const donor7 = { viaProto: 3 };
const r7 = {};
let c7 = 0;
for (let i = 0; i < 300; i = i + 1) c7 = c7 + (r7.viaProto === undefined ? 1 : 0);
console.log(c7);
r7.__proto__ = donor7;
console.log(r7.viaProto);

// --- 8. a null-prototype receiver: the chain ends at once ------------------
const r8 = Object.create(null);
let c8 = 0;
for (let i = 0; i < 300; i = i + 1) c8 = c8 + (r8.nothing === undefined ? 1 : 0);
console.log(c8);
r8.nothing = 4;
console.log(r8.nothing);

// --- 9. a PROXY receiver is never cached -----------------------------------
// 10.5.8 makes every read a trap call; an entry that answered `undefined`
// inline would stop the trap from running at all.
let traps = 0;
const r9 = new Proxy(
    {},
    {
        get: function () {
            traps = traps + 1;
            return undefined;
        },
    }
);
for (let i = 0; i < 50; i = i + 1) {
    if (r9.probe === undefined) traps = traps + 0;
}
console.log(traps);

// --- 10. a DICTIONARY-mode receiver is never cached ------------------------
// The delete moves `r10` to dictionary mode, whose slots are not shape-indexed
// and whose shape belongs to one object.
const r10 = { a: 1, b: 2 };
delete r10.a;
let c10 = 0;
for (let i = 0; i < 300; i = i + 1) c10 = c10 + (r10.later === undefined ? 1 : 0);
console.log(c10);
r10.later = 6;
console.log(r10.later);

// --- 11. `undefined` as a VALUE is not absence -----------------------------
const r11 = { present: undefined };
let c11 = 0;
for (let i = 0; i < 300; i = i + 1) c11 = c11 + (r11.present === undefined ? 1 : 0);
console.log(c11);
console.log("present" in r11);
console.log("gone" in r11);
r11.gone = 1;
console.log(r11.gone);

// --- 12. absent reads interleaved with allocation --------------------------
// The install runs after a walk that may have collected, so it has to reach
// the receiver through the root it was handed. Shaped for the suite's
// BRONZE_GC_STRESS re-run, where every one of these allocations collects.
class Churn {}
let total = 0;
for (let i = 0; i < 500; i = i + 1) {
    const fresh = new Churn();
    const junk = { a: i, b: i + 1, c: [i, i, i] };
    if (fresh.absentKey === undefined) total = total + 1;
    if (junk.alsoAbsent === undefined) total = total + 1;
    if (junk.c[1] !== i) total = total + 1000;
}
console.log(total);

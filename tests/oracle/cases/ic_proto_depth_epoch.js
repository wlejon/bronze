// The depth > 0 inherited-read cache, and every way its chain can change:
// an add to an INTERMEDIATE prototype shadows the cached holder (the epoch is
// the only thing that can notice — the receiver's shape does not change), a
// deeper write behind the shadow changes nothing observable, and a delete
// puts the intermediate in dictionary mode, which the cached walk must refuse
// so the read falls back to the true chain. Warm loops around each change so
// a stale hit would answer with the wrong constant many times over.
class A {}
A.prototype.k = 1;
class B extends A {}
class C extends B {}
const o = new C();

let sum = 0;
for (let i = 0; i < 200; i = i + 1) sum = sum + o.k;
console.log(sum);

B.prototype.k = 50;
let sum2 = 0;
for (let i = 0; i < 200; i = i + 1) sum2 = sum2 + o.k;
console.log(sum2);

A.prototype.k = 7;
console.log(o.k);

delete B.prototype.k;
console.log(o.k);

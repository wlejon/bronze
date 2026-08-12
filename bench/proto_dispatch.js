// An inherited property read at depth 3, with no property adds in the loop:
// the prototype-mutation epoch is stable, so every read should take the
// cached proto hit rather than walking the chain.
//
// Nothing in bench/ covered a depth > 0 read before docs/0032 — property_access
// is two own properties, which generated code inlines and which the epoch
// never touches — so a change that killed proto caching outright could not
// have moved a number here. That is why this file exists beside its churn
// variant rather than as a paragraph in the log.
class A {}
A.prototype.k = 1;
class B extends A {}
class C extends B {}
const o = new C();
let sum = 0;
for (let i = 0; i < 3000000; i = i + 1) {
  sum = sum + o.k;
}
console.log(sum);

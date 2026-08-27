// An inherited property read at depth 3, with no property adds in the loop:
// the prototype-mutation epoch is stable, so every read should take the
// cached proto hit rather than walking the chain.
//
// Nothing in bench/ covered a depth > 0 read before this — property_access is
// two own properties, which generated code inlines and which the epoch never
// touches — so a change that killed proto caching outright could not have moved
// a number here. That is why this file exists beside its churn variant rather
// than as a paragraph in the log.
import { measure } from './harness.js';

class A {}
A.prototype.k = 1;
class B extends A {}
class C extends B {}
const o = new C();

// The loop is a function so the clock can bracket it and nothing else. It is
// also what the sampler needs: at top level every frame of this file is
// `bronze_main`, and a profile that says "main" says nothing.
function readInherited(n) {
  let sum = 0;
  for (let i = 0; i < n; i = i + 1) {
    sum = sum + o.k;
  }
  return sum;
}

console.log(measure('proto_dispatch', () => readInherited(3000000), 3000000));

// Dynamic property access in a real loop: shape lookups + inline caches
// are the whole cost.
import { measure } from './harness.js';

const obj = { a: 1, b: 2 };

function sumProps(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}

console.log(measure('property_access', () => sumProps(obj, 1000000), 1000000));

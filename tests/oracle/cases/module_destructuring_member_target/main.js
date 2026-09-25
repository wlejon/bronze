// A destructuring ASSIGNMENT may name a property reference as its target
// (`({ a: obj[k] } = src)`), and the object and key of that reference are
// ordinary expressions of this module. When `obj` is an import, the linker has
// moved it to the exporting module's binding, and every position a target can
// stand in must follow: object and array patterns, nested patterns, targets
// with defaults, and rest elements of both kinds.
import { obj, arr, keyOf, calls } from './store.js';

const src = { a: 1, b: [2, 3, 4], c: undefined, n: { deep: 5 } };

({ a: obj[keyOf('a')] } = src);
({ c: obj.withDefault = 'dflt' } = src);
({ n: { deep: obj.deep } } = src);
({ b: [arr[0], ...arr[1]] } = src);
[obj.first, obj['second'], ...obj.rest] = [10, 20, 30, 40];
({ a: obj.skipped, ...obj.restObj } = { a: 0, u: 1, v: 2 });
[[obj.nested] = [99]] = [];

console.log(JSON.stringify(obj));
console.log(JSON.stringify(arr));
console.log(calls.length, calls.join(','));

// A function-local `obj` shadows the import: its targets are the local's.
function local() {
  const obj = {};
  ({ a: obj.a } = src);
  return obj;
}
console.log(JSON.stringify(local()), 'a' in obj);

// `import()` in the positions only a pattern or a parameter list has: a
// destructuring default, a declaration default, a parameter default, and a
// class field. Each resolves to the module's one namespace object.
import * as store from './store.js';
const holder = {};
({ missing: holder.p = import('./store.js') } = {});
const { alsoMissing = import('./store.js') } = {};
async function load(m = import('./store.js')) {
  return m;
}
class Lazy {
  static mod = import('./store.js');
}
Promise.all([holder.p, alsoMissing, load(), Lazy.mod]).then((all) => {
  console.log(all.map((ns) => ns === store).join(','));
});

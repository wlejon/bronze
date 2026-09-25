// A for-in/of head that assigns through a property reference of an IMPORTED
// object: the module rename must reach the object inside the head, in the
// plain, destructuring, for-in, generator and async forms.
import { store, list, keyOf } from './store.js';

for (store.last of [1, 2, 3]) {}
for ([store.pair, list[0]] of [['p', 'q']]) {}
for ({ v: store[keyOf('v')] } of [{ v: 'computed' }]) {}
for (store.prop in { only: 1 }) {}
console.log(JSON.stringify(store), JSON.stringify(list));

function* gen() {
  for (store[yield 'k?'] of ['g']) {}
}
const it = gen();
it.next();
it.next('fromYield');

async function run() {
  for (list[await Promise.resolve(1)] of ['async']) {}
}
run().then(() => console.log(JSON.stringify(store), JSON.stringify(list)));

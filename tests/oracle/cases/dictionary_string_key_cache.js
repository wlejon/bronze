// A cache keyed by computed strings, in dictionary mode.
//
// An object given fresh computed keys without end leaves the transition tree
// for a dictionary. A read or an overwrite of a key it holds is then the
// entry's slot, found by the key's text; an absent key still asks the
// prototype chain, an accessor entry still runs, a read-only entry still
// refuses the store, a frozen cache keeps its values, and an index spelled
// as a string names what the number names.

const cache = {};
const bare = Object.create(null);
let hits = 0, sum = 0;
for (let i = 0; i < 3000; i++) {
  const k = 'key-' + (i % 700) + '-' + (i & 3);
  if (cache[k]) { hits++; cache[k].n++; } else { cache[k] = { n: 1 }; }
  bare[k] = (bare[k] || 0) + i;
  if (i % 7 === 0) { cache['key-' + (i % 50) + '-0'] = null; delete bare['key-' + (i % 90) + '-1']; }
}
for (const k in bare) sum += bare[k];
console.log(hits, Object.keys(cache).length, Object.keys(bare).length, sum);
const name = ['to', 'String'].join('');
console.log(typeof cache[name], cache['has' + 'OwnProperty'] === Object.prototype.hasOwnProperty, bare[name]);
cache[name] = 7; console.log(cache[name], cache.toString);
Object.defineProperty(cache, 'acc', { get() { return 'got'; }, set(v) { sum = v; }, configurable: true });
const acc = 'a' + 'cc'; console.log(cache[acc]); cache[acc] = 99; console.log(sum, cache[acc]);
Object.defineProperty(cache, 'ro', { value: 1, writable: false });
const ro = 'r' + 'o'; cache[ro] = 2; console.log(cache[ro]);
Object.freeze(bare); const fk = 'key-' + 3 + '-' + 3; const before = bare[fk]; bare[fk] = -1; console.log(bare[fk] === before);
const idx = {}; for (let i = 0; i < 100; i++) idx['' + i * 3] = i; console.log(idx[30], idx['30'], idx['3' + '3'], idx[31]);

// String keys built by concatenation and used at once — `cells.get(x + ',' +
// y)`, `seen[unit.id + ':' + cell]`, `hashSeed(seed + name)` — the way a sim
// addresses a hex grid and its per-tick caches.
//
// Every key is `dynamic_add` → `string_concat` → a hash of the fresh string →
// a Map probe or a property lookup that compares by content against the
// stored key. This fixture is the rest of the sim's tick bill after the
// iterator and destructuring costs, and it is here to keep that number in
// view: string building is allocation and hashing, and the only inline answer
// is to do less of it.
//
// `numeric_key` is the control: the same grid addressed by `x * 4096 + y`.

import { measure } from './harness.js';

const W = 64, H = 64;
const N = 1500000;

const byString = new Map();
const byNumber = new Map();
for (let y = 0; y < H; y++) {
  for (let x = 0; x < W; x++) {
    const cell = { x, y, cost: (x * 31 + y * 17) % 5 };
    byString.set(x + ',' + y, cell);
    byNumber.set(x * 4096 + y, cell);
  }
}

function stringKeyGet() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const x = (i * 7) & 63, y = (i * 13) & 63;
    const c = byString.get(x + ',' + y);
    acc += c.cost;
  }
  return acc;
}

function objectKeyGet() {
  const seen = {};
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const x = (i * 7) & 63, y = (i * 13) & 63;
    const k = 'u' + (i & 15) + ':' + x + ':' + y;
    const prev = seen[k];
    seen[k] = (prev === undefined ? 0 : prev) + 1;
    acc += prev === undefined ? 1 : 0;
  }
  return acc;
}

function numericKeyGet() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const x = (i * 7) & 63, y = (i * 13) & 63;
    const c = byNumber.get(x * 4096 + y);
    acc += c.cost;
  }
  return acc;
}

const a = measure('string_key_map_get', stringKeyGet, N);
const b = measure('string_key_object_count', objectKeyGet, N);
const c = measure('numeric_key_map_get', numericKeyGet, N);
console.log('checksum ' + a + ' ' + b + ' ' + c);

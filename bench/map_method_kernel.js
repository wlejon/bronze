// The DISPATCH of a keyed collection's methods: `m.get(k)`, `m.has(k)`,
// `m.set(k, v)`, `s.add(v)`, `s.has(v)`, `wm.get(o)` — the calls three.js's
// renderer makes per object per frame (`WebGLProperties`, `WebGLAttributes`
// read `.get` and `.set` off a WeakMap) and a game sim makes per entity per
// tick. The tables are tiny and every probe hits, so the hash walk is a few
// nanoseconds and what the figure mostly measures is how the method is FOUND
// — the read of `get` off the receiver and the call that follows — which is
// the one part of the operation that is a fact about the runtime's object
// model rather than about the key.
//
// `plain_method` is the control: the same loop over an ordinary class
// instance whose `get` is a prototype method, which is what the collection
// dispatch is being asked to be as fast as.

import { measure } from './harness.js';

const N = 10000000;

const map = new Map();
const set = new Set();
const weak = new WeakMap();
const keys = [];
for (let i = 0; i < 16; i++) {
  map.set(i, i * 3);
  set.add(i);
  const o = { id: i };
  keys.push(o);
  weak.set(o, i * 5);
}

class Table {
  constructor() { this.slots = [0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45]; }
  get(k) { return this.slots[k]; }
  has(k) { return k < 16; }
}
const table = new Table();

function mapGet() {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += map.get(i & 15);
  return acc;
}

function mapHas() {
  let acc = 0;
  for (let i = 0; i < N; i++) if (map.has(i & 15)) acc++;
  return acc;
}

function mapSet() {
  for (let i = 0; i < N; i++) map.set(i & 15, i);
  return map.get(7);
}

function setAddHas() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    set.add(i & 15);
    if (set.has(i & 15)) acc++;
  }
  return acc;
}

function weakMapGet() {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += weak.get(keys[i & 15]);
  return acc;
}

function plainMethod() {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += table.get(i & 15);
  return acc;
}

const a = measure('map_get', mapGet, N);
const b = measure('map_has', mapHas, N);
const c = measure('map_set', mapSet, N);
const d = measure('set_add_has', setAddHas, N);
const e = measure('weakmap_get', weakMapGet, N);
const f = measure('plain_method', plainMethod, N);
console.log('checksum ' + a + ' ' + b + ' ' + c + ' ' + d + ' ' + e + ' ' + f);

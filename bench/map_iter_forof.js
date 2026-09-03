// for-of over a Map's ITERATOR OBJECT — `map.values()`, `map.keys()`,
// `map.entries()` — and `Array.from(map.values())`, which is what a game sim
// does on every tick to walk its unit table (TATS: `for (const unit of
// sim.units.values())` in every phase; `Array.from(sim.units.values()).filter(...)`
// for a company's roster).
//
// Before: a Map iterator object was a protocol iteration — one call to its
// native `next` per element, a `{value, done}` object built by name (two
// `setProp`s, two transition scans, two arena copies of the key) and two
// property reads to take it apart — 62 % of the sim's tick-loop samples.
// After: `rtOpenIterator` recognises a pristine built-in iterator object and
// steps its internal slots directly (runtime/iterator.h `MapIterator`), which
// is what `for (const [k, v] of map)` already got.
//
// The plain `for (const [k, v] of map)` region is the control: the cursor
// walk that was always fast, so the gap between it and `values()` is the
// whole cost of the iterator object.

import { measure } from './harness.js';

const UNITS = 400;
const TICKS = 2000;

const units = new Map();
for (let i = 0; i < UNITS; i++) {
  units.set('u' + i, { id: 'u' + i, hp: 100 + (i % 7), x: i * 3, z: i * 5, company: i % 2 ? 'a' : 'b', alive: true });
}

function viaValues() {
  let acc = 0;
  for (let t = 0; t < TICKS; t++) {
    for (const u of units.values()) acc += u.hp + (u.x & 3);
  }
  return acc;
}

function viaKeys() {
  let acc = 0;
  for (let t = 0; t < TICKS; t++) {
    for (const k of units.keys()) acc += k.length;
  }
  return acc;
}

function viaEntries() {
  let acc = 0;
  for (let t = 0; t < TICKS; t++) {
    for (const [k, u] of units.entries()) acc += k.length + u.z;
  }
  return acc;
}

function viaMapDirect() {
  let acc = 0;
  for (let t = 0; t < TICKS; t++) {
    for (const [k, u] of units) acc += k.length + u.z;
  }
  return acc;
}

function viaArrayFrom() {
  let acc = 0;
  for (let t = 0; t < TICKS / 4; t++) {
    const mine = Array.from(units.values()).filter(u => u.company === 'a' && u.alive);
    acc += mine.length;
  }
  return acc;
}

const a = measure('map_values_forof', viaValues, TICKS * UNITS);
const b = measure('map_keys_forof', viaKeys, TICKS * UNITS);
const c = measure('map_entries_forof', viaEntries, TICKS * UNITS);
const d = measure('map_direct_forof', viaMapDirect, TICKS * UNITS);
const e = measure('map_array_from_filter', viaArrayFrom, (TICKS / 4) * UNITS);
console.log('checksum ' + (a + b + c + d + e));

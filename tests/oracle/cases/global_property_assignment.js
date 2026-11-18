// An assignment to a name no binding of the program holds is a [[Set]] on
// the global object, with that property's own rules (9.1.1.4.5, 10.1.9.2):
// a read-only accessor or a non-writable data property is a silent no-op in
// sloppy code and a TypeError in strict code, a writable one takes the value,
// and a builtin global (writable, configurable) can be replaced. hextris's
// `history = {}` on the browser's setter-less `history` was a compile error
// that rejected the whole script.

Object.defineProperty(globalThis, 'readOnlyAccessor', { get() { return 'host'; }, configurable: true });
Object.defineProperty(globalThis, 'frozenData', { value: 1, writable: false, configurable: true });
let backing = 0;
Object.defineProperty(globalThis, 'replaceable', {
  get() { return backing; },
  set(v) { backing = v; },
  configurable: true,
});

function sloppyInit() {
  readOnlyAccessor = {};
  frozenData = 2;
  replaceable = 'set';
  return [typeof readOnlyAccessor, readOnlyAccessor, frozenData, replaceable, backing].join(' ');
}
console.log(sloppyInit());

function strictInit() {
  'use strict';
  const out = [];
  try { readOnlyAccessor = {}; out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
  try { frozenData = 3; out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
  try { replaceable = 'strict'; out.push(replaceable); } catch (e) { out.push(e.constructor.name); }
  return out.join(' ');
}
console.log(strictInit());

// Compound and update forms read first, then put by the same rules.
function compound() {
  frozenData += 5;
  frozenData++;
  replaceable += '!';
  return frozenData + ' ' + replaceable;
}
console.log(compound());

// A builtin global is an ordinary writable property: replacing one is seen
// by later bare reads, and putting it back restores it.
const savedJSON = JSON;
function swapBuiltin() {
  JSON = { stringify() { return 'mine'; } };
  const seen = JSON.stringify(1);
  JSON = savedJSON;
  return seen + ' ' + JSON.stringify([1]);
}
console.log(swapBuiltin());

// The three read-only value properties.
function valueProps() {
  undefined = 1;
  NaN = 2;
  Infinity = 3;
  return [typeof undefined, NaN, Infinity].join(' ');
}
console.log(valueProps());

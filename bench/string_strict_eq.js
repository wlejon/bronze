// `===` with a STRING on the left — `unit.kind === 'titan'`, `order.type ===
// 'move'`, `a.company === b.company` — over records whose string fields are
// not interned, so equal strings are different objects and the compare is by
// content. A sim's rules are made of these.
//
// Before: the inline strict-eq answered numbers and same-pointer, and sent
// every string on the left to the helper — which for the common MISS (`'move'`
// against `'hold'`) is a call to find out the lengths differ, and for a
// non-string on the right a call to find out the tags differ.
// After: the inline path reads both headers' length words and answers a
// different length or a non-string right operand itself; only two strings of
// one length call the helper for the content compare.
//
// `enum_number_eq` is the control: the same dispatch over small integers.

import { measure } from './harness.js';

const N = 2000000;
const KINDS = ['walker', 'tracked', 'titan', 'colossus', 'superheavy', 'minion'];
const ORDERS = ['move', 'hold', 'attack', 'retreat', 'scan', 'callDown'];

// Distinct string objects per record: built by concatenation so nothing is
// shared with the literals the compare sites hold.
function fresh(s) { return (s + '#').slice(0, s.length); }

const units = [];
for (let i = 0; i < 256; i++) {
  units.push({ kind: fresh(KINDS[i % KINDS.length]), order: fresh(ORDERS[(i * 7) % ORDERS.length]),
               kindNo: i % KINDS.length, orderNo: (i * 7) % ORDERS.length, target: i & 1 ? null : fresh('u' + i) });
}

function stringDispatch() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const u = units[i & 255];
    if (u.order === 'move') acc += 1;
    else if (u.order === 'hold') acc += 2;
    else if (u.order === 'attack') acc += 3;
    else if (u.order === 'retreat') acc += 4;
    else acc += 5;
    if (u.kind === 'titan' || u.kind === 'colossus') acc += 10;
    if (u.target === null) acc += 100;
  }
  return acc;
}

function stringMismatchRight() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const u = units[i & 255];
    // Left is a string, right is not: a helper call before, a tag test now.
    if (u.kind === u.kindNo) acc += 1;
    if (u.order === undefined) acc += 2;
    acc += (u.kind === 'walker') ? 1 : 0;
  }
  return acc;
}

function enumNumberEq() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const u = units[i & 255];
    if (u.orderNo === 0) acc += 1;
    else if (u.orderNo === 1) acc += 2;
    else if (u.orderNo === 2) acc += 3;
    else if (u.orderNo === 3) acc += 4;
    else acc += 5;
    if (u.kindNo === 2 || u.kindNo === 3) acc += 10;
  }
  return acc;
}

const a = measure('string_dispatch', stringDispatch, N);
const b = measure('string_mismatch_right', stringMismatchRight, N);
const c = measure('enum_number_eq', enumNumberEq, N);
console.log('checksum ' + a + ' ' + b + ' ' + c);

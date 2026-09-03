// Bitwise operators over DYNAMIC operands: a seeded hash that walks a string
// with `charCodeAt` and mixes with `^ * >>>`, and an xorshift PRNG whose
// state lives in a closure's environment slot. Both are what a deterministic
// game keeps in every hot loop (TATS: `hashSeed`, `rng`), and neither lets
// inference pin the operand to a number — the hash's accumulator crosses a
// string method's result, the PRNG's state is a captured `let`.
//
// Before: every dynamic `&`, `|`, `^`, `<<`, `>>`, `>>>` was a helper call that
// went through ToNumeric on both sides — a root, a ToPrimitive walk and a
// ToNumber — for what was two numbers; 28 % of the sim's whole-run samples.
// After: two number operands are ToInt32'd and combined inline
// (codegen-llvm/llvm_arith.cpp), and the helper is the non-number edge only.
//
// `typed_xorshift` is the control: the same generator with its state in a
// local the inferencer pins to a number, which is what the dynamic one should
// approach.

import { measure } from './harness.js';

const N = 4000000;

function hashSeed(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619) >>> 0;
    h = (h ^ (h >>> 13)) & 0xffffffff;
  }
  return h >>> 0;
}

function rng(seed) {
  let a = seed >>> 0 || 1;
  return () => {
    a ^= a << 13; a >>>= 0;
    a ^= a >>> 17;
    a ^= a << 5; a >>>= 0;
    return a / 4294967296;
  };
}

function hashKeys() {
  let acc = 0;
  const keys = [];
  for (let i = 0; i < 64; i++) keys.push('unit-' + i + ':cell:' + (i * 37));
  for (let i = 0; i < N / 64; i++) {
    for (let k = 0; k < 64; k++) acc = (acc + hashSeed(keys[k])) >>> 0;
  }
  return acc;
}

function closureXorshift() {
  const r = rng(1337);
  let acc = 0;
  for (let i = 0; i < N; i++) acc += r() < 0.5 ? 1 : (i & 7);
  return acc;
}

function typedXorshift() {
  let a = 1337;
  let acc = 0;
  for (let i = 0; i < N; i++) {
    a ^= a << 13; a >>>= 0;
    a ^= a >>> 17;
    a ^= a << 5; a >>>= 0;
    acc += (a / 4294967296) < 0.5 ? 1 : (i & 7);
  }
  return acc;
}

const a = measure('dynamic_hash_string', hashKeys, N);
const b = measure('closure_xorshift', closureXorshift, N);
const c = measure('typed_xorshift', typedXorshift, N);
console.log('checksum ' + a + ' ' + b + ' ' + c);

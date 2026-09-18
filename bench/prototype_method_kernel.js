// The DISPATCH of a typed array's and a RegExp's prototype methods:
// `v.subarray(a, b)`, `dst.set(src, off)`, `v.slice(a, b)`, `re.test(s)` and
// `re.exec(s)` — the calls three.js makes per attribute upload
// (`BufferAttribute.set`, `subarray` for interleaved views), a serializer
// makes per record, and a tokenizer makes per token. The work under each
// call is a few dozen bytes or a few characters, so what the figure mostly
// measures is how the method is FOUND — the read of `set` off a view that
// now has a real `%TypedArray%.prototype` above it, or of `test` off a
// RegExp with a real `RegExp.prototype` — and the call that follows, which
// is the one part of the operation that is a fact about the runtime's
// object model rather than about the bytes.
//
// `plain_method` is the control: the same loop over an ordinary class
// instance whose `slice` is a prototype method, which is what the native
// dispatch is being asked to be as fast as. `typed_index` is the other
// control: indexed element access on the same view, which must not move at
// all when the named path does.

import { measure } from './harness.js';

const N = 2000000;

const src = new Float32Array(64);
for (let i = 0; i < 64; i++) src[i] = i * 0.25;
const dst = new Float32Array(128);
const words = ['alpha', 'beta12', 'gamma', 'delta4', 'x', 'epsilon99'];
const digits = /\d+/;
const global = /a/g;

class Table {
  constructor() { this.slots = src; }
  slice(a, b) { return this.slots[a] + this.slots[b - 1]; }
}
const table = new Table();

function subarray() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const w = src.subarray(i & 31, (i & 31) + 8);
    acc += w.length;
  }
  return acc;
}

function set() {
  for (let i = 0; i < N; i++) dst.set(src, i & 63);
  return dst[70];
}

function slice() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    const w = src.slice(i & 31, (i & 31) + 4);
    acc += w[0];
  }
  return acc;
}

function reTest() {
  let acc = 0;
  for (let i = 0; i < N; i++) if (digits.test(words[i % 6])) acc++;
  return acc;
}

function reExec() {
  let acc = 0;
  for (let i = 0; i < N; i++) {
    global.lastIndex = 0;
    const m = global.exec(words[i % 6]);
    if (m !== null) acc += m.index;
  }
  return acc;
}

function typedIndex() {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += src[i & 63];
  return acc;
}

function plainMethod() {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += table.slice(i & 31, (i & 31) + 4);
  return acc;
}

// The string-side dispatch: `str.replace(/re/g, "x")` finds `[Symbol.replace]`
// on the RegExp argument, and a RegExp whose chain is as built is answered
// with no property read at all (`rtRegExpChainPristine`). This is the path
// a templating loop and a tokenizer's normaliser run.
function strReplace() {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += words[i % 6].replace(global, 'A').length;
  return acc;
}

// `Function.prototype.call` read off a plain function: `call` is an own
// property of `Function.prototype`'s statics box, found at the end of the
// function-receiver ladder, which is what a `super`-style helper and every
// `Array.prototype.slice.call(arguments)` idiom pays per call.
function addTo(a, b) { return this.base + a + b; }
const ctx = { base: 1 };
function fnCall() {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += addTo.call(ctx, i & 7, 1);
  return acc;
}

const a = measure('ta_subarray', subarray, N);
const b = measure('ta_set', set, N);
const c = measure('ta_slice', slice, N);
const d = measure('re_test', reTest, N);
const e = measure('re_exec', reExec, N);
const f = measure('typed_index', typedIndex, N);
const g = measure('plain_method', plainMethod, N);
const h = measure('str_replace', strReplace, N);
const k = measure('fn_call', fnCall, N);
console.log('checksum ' + a + ' ' + b + ' ' + c + ' ' + d + ' ' + e + ' ' + f + ' ' + g + ' ' + h +
  ' ' + k);

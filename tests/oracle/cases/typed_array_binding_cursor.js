// A typed array read from a captured or module binding in a buffer-safe
// function gets a cursor (data pointer + length) taken right after the read
// (il_property_lowering.cpp get_preloaded_ta_cursor, is_binding_read). These
// are the ways a binding could name something else, or an array whose
// storage or length changed, between one call and the next.

// The N-body shape: closures over module-level arrays.
const pos = new Float64Array(8), vel = new Float64Array(8);
for (let i = 0; i < 8; i++) { pos[i] = i; vel[i] = 0.5 * i; }
function step(dt) { for (let i = 0; i < pos.length; i++) pos[i] += vel[i] * dt; }
for (let k = 0; k < 10; k++) step(0.1);
console.log(Array.from(pos).join(","));

// A captured binding reassigned between calls: each call's cursor is the
// array the binding holds then, of its own length.
function makeSummer() {
  let buf = new Int32Array([1, 2, 3]);
  return {
    sum() { let s = 0; for (let i = 0; i < buf.length; i++) s += buf[i]; return s; },
    set(b) { buf = b; },
    write(v) { for (let i = 0; i < buf.length; i++) buf[i] = v; },
  };
}
const m = makeSummer();
console.log(m.sum());
m.set(new Int32Array([10, 20, 30, 40, 50]));
console.log(m.sum());
m.write(7); console.log(m.sum());
m.set(new Int32Array(0)); console.log(m.sum());
// The binding now holds something that is not a typed array at all.
m.set([1, 2, "3"]); console.log(m.sum());
m.set({ length: 2, 0: 4, 1: 5 }); console.log(m.sum());
m.set(new Float32Array([0.5, 0.25])); console.log(m.sum());

// A detached buffer: the length a cursor reads is 0 and stores go nowhere.
let det = new Uint8Array(new ArrayBuffer(4));
function fill(v) { for (let i = 0; i < 4; i++) det[i] = v; return det[0] + det.length; }
console.log(fill(3));
const moved = det.buffer.transfer();
console.log(fill(9), det.length, new Uint8Array(moved).join(","));

// A length-tracking view on a resizable buffer: the length is the buffer's now.
const rab = new ArrayBuffer(4, { maxByteLength: 16 });
let track = new Uint8Array(rab);
function count() { let n = 0; for (let i = 0; i < track.length; i++) { track[i] = i + 1; n += track[i]; } return n; }
console.log(count());
rab.resize(10); console.log(count());
rab.resize(2); console.log(count(), track.length);

// An out-of-range index through the cursor reads undefined and writes nothing.
const small = new Float64Array(2);
function edge() { small[5] = 1; small[-1] = 2; small[1.5] = 3; return [small[5], small[-1], small[1], small.length]; }
console.log(edge());

// Garbage made between calls moves nothing a later call's cursor depends on.
let churn = new Float64Array(64);
function total() { let s = 0; for (let i = 0; i < churn.length; i++) s += churn[i]; return s; }
let acc = 0;
for (let r = 0; r < 50; r++) {
  const junk = []; for (let j = 0; j < 200; j++) junk.push({ j, s: "x" + j });
  churn = new Float64Array(64).fill(r);
  acc += total() + junk.length;
}
console.log(acc);

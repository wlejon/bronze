// 10.4.5 length-tracking views: a typed array constructed with no length over
// a resizable buffer follows every resize (floor((byteLength - byteOffset) /
// elementSize)), strands only when the buffer drops below its OFFSET, and
// reopens by recomputation rather than to a remembered count. Also pinned
// here: subarray's tracking propagation and its no-ValidateTypedArray
// semantics (23.2.3.30), the constructor measuring the buffer AFTER the
// length conversion (23.2.5.1), growable SharedArrayBuffer tracking, and the
// auto-length DataView (25.3.2.1 step 10).
function kind(fn) {
  try { var v = fn(); return "ok:" + v; }
  catch (e) {
    if (e instanceof TypeError) return "TypeError";
    if (e instanceof RangeError) return "RangeError";
    return "other:" + e;
  }
}

// --- tracking follows the buffer in both directions ---
var rab = new ArrayBuffer(8, { maxByteLength: 32 });
var ta = new Uint8Array(rab);
console.log(ta.length);            // 8
rab.resize(16);
console.log(ta.length);            // 16
rab.resize(4);
console.log(ta.length);            // 4
rab.resize(8);
console.log(ta.length);            // 8

// --- a nonzero offset: floor of the tail, stranded only below the offset ---
var tb = new Uint16Array(rab, 2);
console.log(tb.length);            // (8-2)/2 = 3
rab.resize(12);
console.log(tb.length);            // (12-2)/2 = 5
rab.resize(2);
// at the offset exactly: EMPTY, not out of bounds — methods answer emptily
console.log(tb.length, kind(function(){ return tb.join(","); }));  // 0 ok:
rab.resize(1);
// below the offset: out of bounds — methods throw, getters answer 0
console.log(tb.length, tb.byteLength, tb.byteOffset,
            kind(function(){ return tb.join(","); }));             // 0 0 0 TypeError
rab.resize(8);
console.log(tb.length, tb.byteOffset);                             // 3 2

// --- a fixed-length view over the same buffer does NOT track ---
var tc = new Uint8Array(rab, 0, 4);
rab.resize(16);
console.log(tc.length);            // 4
rab.resize(2);
console.log(tc.length);            // 0 (stranded)
rab.resize(8);
console.log(tc.length);            // 4 (reopened to its constructed count)

// --- subarray: tracking propagates when end is omitted, not otherwise ---
var sub = ta.subarray(2);
console.log(sub.length);           // 6
rab.resize(16);
console.log(sub.length);           // 14
var fixedSub = ta.subarray(1, 5);
rab.resize(32);
console.log(fixedSub.length);      // 4 (explicit end: fixed)
rab.resize(3);
console.log(fixedSub.length, sub.length);  // 0 1
rab.resize(8);

// --- subarray performs no ValidateTypedArray: stranded answers, detached
// --- reaches the constructor's TypeError ---
var buf2 = new ArrayBuffer(8, { maxByteLength: 16 });
var te = new Uint8Array(buf2, 0, 8);
buf2.resize(2);
console.log(kind(function(){ return te.subarray(0, 2).length; })); // ok:0
var db = new ArrayBuffer(8);
var dta = new Uint8Array(db);
db.transfer();
console.log(kind(function(){ return dta.subarray(0); }));          // TypeError

// --- element access and iteration see the grown window ---
var g1 = new ArrayBuffer(4, { maxByteLength: 12 });
var tv = new Uint8Array(g1);
g1.resize(12);
for (var i = 0; i < tv.length; i++) tv[i] = i + 1;
var total = 0;
for (var j = 0; j < tv.length; j++) total += tv[j];
console.log(tv.length, total);     // 12 78

var g2 = new ArrayBuffer(2, { maxByteLength: 4 });
var gv = new Uint8Array(g2);
gv[0] = 1; gv[1] = 2;
var seen = [];
for (var x of gv) {
  seen.push(x);
  if (seen.length === 1) { g2.resize(4); gv[2] = 3; gv[3] = 4; }
}
console.log(seen.join(","));       // 1,2,3,4 — the iterator asks the live length

// --- methods over the grown window ---
var g3 = new ArrayBuffer(2, { maxByteLength: 6 });
var mv = new Uint8Array(g3);
mv[0] = 9; mv[1] = 8;
g3.resize(4);
mv.fill(7, 2);
console.log(mv.join(","), mv.at(-1));  // 9,8,7,7 7

// --- growable SharedArrayBuffer: same tracking, grow() refreshes ---
var sab = new SharedArrayBuffer(4, { maxByteLength: 8 });
var ts = new Uint8Array(sab);
sab.grow(8);
var sab2 = new SharedArrayBuffer(4);
var ts2 = new Uint8Array(sab2);
console.log(ts.length, ts2.length);    // 8 4

// --- the constructor: divisibility binds only a FIXED buffer's tail;
// --- a resizable one floors instead ---
var fixed6 = new ArrayBuffer(6);
console.log(kind(function(){ return new Float32Array(fixed6); }));  // RangeError
var rag = new ArrayBuffer(6, { maxByteLength: 8 });
var f32 = new Float32Array(rag);
console.log(f32.length);           // 1
rag.resize(8);
console.log(f32.length);           // 2
var fb = new ArrayBuffer(10, { maxByteLength: 16 });
var f64 = new Float64Array(fb);
console.log(f64.length, f64.byteLength);  // 1 8

// --- the constructor measures the buffer AFTER ToIndex(length): a valueOf
// --- that resizes is judged against the buffer as it is NOW ---
var vg = new ArrayBuffer(8, { maxByteLength: 32 });
console.log(kind(function(){
  return new Uint8Array(vg, 0, { valueOf: function(){ vg.resize(16); return 12; } }).length;
}));                               // ok:12
var vd = new ArrayBuffer(8);
console.log(kind(function(){
  return new Uint8Array(vd, 0, { valueOf: function(){ vd.transfer(); return 4; } });
}));                               // TypeError

// --- the auto-length DataView: measured per access, no walk ---
var dab = new ArrayBuffer(8, { maxByteLength: 16 });
var dv = new DataView(dab, 4);
console.log(dv.byteLength);        // 4
dab.resize(2);
console.log(kind(function(){ return dv.byteLength; }),
            kind(function(){ return dv.getUint8(0); }));  // TypeError TypeError
dab.resize(16);
console.log(dv.byteLength);        // 12
dv.setUint8(11, 5);
console.log(dv.getUint8(11));      // 5
var dvf = new DataView(dab, 0, 4);
dab.resize(8);
console.log(dvf.byteLength);       // 4 (explicit length: fixed)

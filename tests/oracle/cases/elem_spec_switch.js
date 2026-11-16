// TRAINING true runs the warm-up alone: a harvest of that run, fed to a
// build of this file, makes every switched call below fail a guard.
const TRAINING = false;
// Element reads and writes trained on one receiver kind, then given others.
function sum(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function first(a) { return a[0] + a[1] * 2 + a[3]; }
function setFirst(a, v) { a[0] = v; a[2] = v + 1; return a; }
function fill(a, v) { for (let i = 0; i < a.length; i++) a[i] = v + i; return a; }
function get(a, i) { return a[i]; }
function put(a, i, v) { a[i] = v; return a; }
function len(a) { return a.length; }
function sumF(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sumD(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sumI(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function firstF(a) { return a[0] + a[1] * 2 + a[3]; }
function getF(a, i) { return a[i]; }
function getD(a, i) { return a[i]; }
function getI(a, i) { return a[i]; }
function putF(a, i, v) { a[i] = v; return a; }
function putD(a, i, v) { a[i] = v; return a; }
function putI(a, i, v) { a[i] = v; return a; }
function fillF(a, v) { for (let i = 0; i < a.length; i++) a[i] = v + i; return a; }
function fillD(a, v) { for (let i = 0; i < a.length; i++) a[i] = v + i; return a; }
function constLoop(a, n) { let s = 0; for (let i = 0; i < n; i++) { s += a[0] + a[1] * 2 + a[3]; a[2] = s % 7; } return s; }
function constLoopF(a, n) { let s = 0; for (let i = 0; i < n; i++) { s += a[0] + a[1] * 2 + a[3]; a[2] = s % 7; } return s; }
function idxLoopI(a, k, n) { let s = 0; for (let i = 0; i < n; i++) { s = (s + a[k]) | 0; a[k ^ 1] = s; } return s; }
function idxLoopD(a, k, n) { let s = 0; for (let i = 0; i < n; i++) { s += a[k]; a[k ^ 1] = s * 0.5; } return s; }
function fillI(a, v) { for (let i = 0; i < a.length; i++) a[i] = v + i; return a; }
const arr = [1, 2, 3, 4, 5, 6, 7, 8];
const f32 = new Float32Array([1.5, 2.5, 3.5, 4.5]);
const f64 = new Float64Array([0.1, 0.2, 0.3, 0.4]);
const i32 = new Int32Array([10, 20, 30, 40]);
let acc = 0;
for (let i = 0; i < 20000; i++) {
  acc += sum(arr) + first(arr) + get(arr, i & 7) + len(arr);
  acc += sumF(f32) + firstF(f32) + getF(f32, i & 3);
  acc += sumD(f64) + getD(f64, i & 3) + sumI(i32) + getI(i32, i & 3);
  setFirst(arr, i & 7); fill(arr, 1); fillF(f32, 1); fillD(f64, 0.5); fillI(i32, 3); put(arr, i & 7, i & 15); putI(i32, i & 3, i);
  putF(f32, i & 3, i * 0.5); putD(f64, i & 3, i * 0.25);
}
acc += constLoop([1, 2, 3, 4, 5], 400000) + constLoopF(new Float32Array([1, 2, 3, 4]), 400000);
acc += idxLoopI(new Int32Array([1, 2, 3, 4]), 2, 400000) + idxLoopD(new Float64Array([1, 2, 3, 4]), 1, 400000);
console.log(acc);
const show = (v) => v === undefined ? 'u' : Object.is(v, -0) ? '-0' : ArrayBuffer.isView(v) || Array.isArray(v) ? '[' + Array.from(v, show).join(',') + ']' : String(v);
if (!TRAINING) {
  console.log(sum(new Float32Array([0.1, 0.2])), sum(new Int32Array([5, 6])), sum([1, , 3]), sum('123'), sum({ length: 2, 0: 4, 1: 5 }));
  console.log(sum([1.5, 'x', 2]), sum(new Uint8Array([200, 100])), sum(new Float64Array(0)), sum([]));
  console.log(first(new Float64Array([1, 2, 3, 4])), first([1, 2]), first([, 2, 3, 4]), first('1234'), first(new Int32Array(4)));
  const proto = [9, 9, 9, 9]; const holey = [1, , 3, 4]; Object.setPrototypeOf(holey, proto);
  console.log(first(holey), get(holey, 1), get([1, 2], -1), get([1, 2], 2), get([1, 2], 1.5), get([1, 2], '1'), get(new Int32Array(2), 5));
  console.log(show(setFirst([1, 2], 7)), show(setFirst(new Float32Array(3), 0.1)), show(setFirst(new Int32Array(3), 2.7)), (() => { try { return show(setFirst(Object.freeze([1, 2, 3]), 5)); } catch (e) { return e.constructor.name; } })());
  console.log(show(fill(new Int32Array(3), 0.5)), show(fill(new Float64Array(2), -0)), show(fill([0, 0], 'a')), show(fill(new Uint8Array(3), 254)));
  console.log(show(put(new Int32Array(2), 0, 'x')), show(put(new Int32Array(2), 1, 2 ** 32 + 5)), show(put(new Float32Array(2), 0, 1e40)), show(put(new Float32Array(2), 5, 1)));
  console.log(show(put([1, 2], 2, 3)), show(put([1, 2], 5, 3)), show(put(new Float64Array(2), 1, { valueOf() { return 9; } })), show(put(new Int32Array(2), 0, -0)));
  const gp = Object.create(Array.prototype); Object.defineProperty(gp, 0, { get() { return 100; }, configurable: true });
  const getter = [, 2, 3, 4]; Object.setPrototypeOf(getter, gp);
  console.log(first(getter), get(getter, 0), sum(getter));
  console.log(len('abcd'), len(new Float32Array(3)), len({ length: 'L' }), len([]), len(function (a, b) {}), len(Object.assign([1, 2], { length: 1 })));
  try { len(null); } catch (e) { console.log(e.constructor.name); }
  try { get(undefined, 0); } catch (e) { console.log(e.constructor.name); }
  console.log(sumF([1, 2]), sumF(new Float64Array([1, 2])), sumD(new Float32Array([0.1])), sumI(new Uint32Array([4294967295])), sumI([, 1]));
  console.log(firstF([1, 2, 3, 4]), getF(f32, 4), getF(f32, -1), getD(f64, 1.5), getI(i32, '2'), getI({ 1: 'o' }, 1), getF('str', 1));
  console.log(show(putF(new Float32Array(2), 2, 1)), show(putD(new Float64Array(2), 0, 'x')), show(putD(new Float64Array(2), 0, '2.5')), show(putI(new Int32Array(2), 0, 1.9)), show(putI(new Int32Array(2), 1, NaN)));
  console.log(show(putI(new Int32Array(2), 0, -2.5)), show(putI(new Int32Array(2), 0, 2 ** 31)), show(putI(new Int32Array(2), 0, -(2 ** 31) - 1)), show(putI([0], 0, 3.5)), show(putF(new Float32Array(1), 0, 1 / 3)));
  console.log(show(fillF(new Float32Array(2), 0.1)), show(fillD([0, 0], 1)), show(fillI(new Int32Array(2), 2 ** 31 - 1)), show(fillI(new Int16Array(2), 40000)), show(fillI(new Int32Array(2), 'q')));
  console.log(constLoop([1, 2], 3), constLoop([1, 2, 3], 2), constLoop(new Float32Array(4), 2), constLoop([, 1, 2, 3], 2), constLoop([1, 2, 3, 4.5], 3), (() => { try { return constLoop(Object.freeze([1, 2, 3, 4]), 1); } catch (e) { return e.constructor.name; } })());
  console.log(constLoopF([1, 2, 3, 4], 3), constLoopF(new Float32Array(2), 2), constLoopF(new Float64Array([1, 2, 3, 4]), 2), constLoopF(new Int8Array(4), 2), (() => { try { return constLoopF('abcd', 1); } catch (e) { return e.constructor.name; } })());
  console.log(idxLoopI(new Int32Array([1, 2, 3, 4]), 3, 5), idxLoopI(new Int32Array(2), 2, 3), idxLoopI([1, 2, 3], 1, 3), idxLoopI(new Int32Array([2 ** 30, 2 ** 30]), 0, 3), idxLoopI(new Int32Array(4), 1.5, 2), idxLoopI(new Uint32Array([4294967295, 1]), 0, 2));
  console.log(idxLoopD(new Float64Array([1, 2]), 0, 4), idxLoopD(new Float64Array(2), 3, 2), idxLoopD(new Float32Array([1, 2]), 0, 4), idxLoopD([1, 2], 1, 3), idxLoopD(new Float64Array([NaN, -0]), 1, 2));
  const ta = new Float32Array(new ArrayBuffer(16), 4, 2); ta[0] = 1.25; ta[1] = 2.5; console.log(sum(ta), first(ta), sumF(ta), firstF(ta));
}

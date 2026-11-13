// Constant-index reads and writes on typed arrays.
//
// `c[0] = r` and `m[12]` lower as a dense Array's element with a call for any
// other receiver. That call asks first whether the receiver is a word view
// (Int32Array, Uint32Array, Float32Array, Float64Array) and answers its
// element in place; every other kind, a string, a sparse array and an object
// with index keys take the keyed path as before. The answers must be the
// same for every kind: stores past the length dropped, reads past it
// undefined, values converted as the kind converts them, a value that is no
// Number converted by ToNumber, a BigInt view's elements left to the helper,
// and a subclass's own accessor never consulted for an index.

function fill(a, x) { a[0] = x; a[1] = x * 2; a[2] = x + 0.5; a[3] = -x; return a; }
function read(a) { return [a[0], a[1], a[2], a[3], a[7]]; }
const kinds = [Float32Array, Float64Array, Int32Array, Uint32Array, Int16Array, Uint8Array, Uint8ClampedArray, Int8Array, Uint16Array];
for (const K of kinds) {
  const a = fill(new K(4), 300.25);
  console.log(K.name, read(a).join(' '));
}
for (const K of kinds) {
  const a = new K(2);          // out of bounds stores are dropped, reads undefined
  fill(a, 7);
  console.log(K.name, read(a).join(' '), a.length);
}
console.log(read(fill([], 3)).join(' '));
console.log(read(fill({}, 3)).join(' '));
console.log(read('abcdefgh').join(' '));
const f = new Float32Array(4);
f[0] = '2.5'; f[1] = null; f[2] = undefined; f[3] = { valueOf() { return 9; } };
console.log(read(f).join(' '));
const i = new Int32Array(4);
i[0] = 4294967297; i[1] = -1.9; i[2] = NaN; i[3] = Infinity;
console.log(read(i).join(' '));
const u = new Uint32Array(1); u[0] = -1; console.log(u[0]);
class V extends Float64Array { get 5() { return 'never'; } }
const v = new V(8); v[5] = 1.5; console.log(v[5], v[6]);
const big = new BigInt64Array(2); big[0] = 5n; console.log(String(big[0]), String(big[1]));
function sum(m) { return m[0] + m[1] + m[2] + m[3] + m[4] + m[5]; }
console.log(sum(new Float32Array([1, 2, 3, 4, 5, 6])), sum([1, 2, 3, 4, 5, 6]), sum(new Int32Array([1, 2, 3, 4, 5, 6])));
let t = 0; for (let k = 0; k < 2000; k++) { const c = new Float32Array(4); fill(c, k); t += c[0] + c[3] + c[1]; } console.log(t);

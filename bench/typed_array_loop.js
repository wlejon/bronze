// The loop the indexed fast path is about: element access on a typed array,
// which is what three.js's `BufferAttribute` inner loops are made of. Every
// read and every write here is a `bronze_elem_get` / `bronze_elem_set` call
// today; the fast path that deletes them is the work decision 4 costs.
//
// The two halves are deliberately comparable: the same arithmetic runs over a
// Float32Array and over a plain JS array, so the difference between the two
// numbers is the difference between the two element paths and not the loop.
function overTypedArray(n) {
  const v = new Float32Array(1024);
  for (let i = 0; i < 1024; i++) v[i] = i * 0.5;
  let sum = 0;
  for (let pass = 0; pass < n; pass++) {
    for (let i = 0; i < 1024; i++) {
      sum = sum + v[i];
      v[i] = v[i] * 1.0000001;
    }
  }
  return sum;
}

function overPlainArray(n) {
  const a = [];
  for (let i = 0; i < 1024; i++) a.push(i * 0.5);
  let sum = 0;
  for (let pass = 0; pass < n; pass++) {
    for (let i = 0; i < 1024; i++) {
      sum = sum + a[i];
      a[i] = a[i] * 1.0000001;
    }
  }
  return sum;
}

console.log(overTypedArray(2000));
console.log(overPlainArray(2000));

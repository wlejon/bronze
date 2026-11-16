// Index-style products `i * k` of loop counters: small for most of the run,
// then a product past int32 and products that are -0 (0 times a negative
// counter). The int32 multiply must give way to the double one whenever the
// exact product is not an int32.
function sumProducts(n, k) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const j = i * k;
    s += j + 1;
  }
  return s;
}
function negZero(n) {
  let count = 0;
  for (let i = 0; i < n; i++) {
    const z = 0 * (i - n);  // -0 for every i < n
    if (Object.is(z, -0)) count++;
  }
  return count;
}
function main() {
  let s = 0;
  for (let r = 0; r < 2000; r++) s += sumProducts(100, 3);
  console.log(s);
  console.log(sumProducts(4, 1 << 30), sumProducts(3, -(1 << 30)), sumProducts(2, 2147483647));
  console.log(negZero(10), 1 / (0 * -3), Object.is(sumProducts(1, -1) - 1, 0));
  const a = new Float32Array(16);
  for (let i = 0; i < 4; i++) for (let k = 0; k < 4; k++) a[k * 4 + i] = k - i;
  console.log(Array.from(a).join(","));
}
main();

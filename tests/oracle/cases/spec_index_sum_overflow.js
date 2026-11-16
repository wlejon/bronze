// An int index sum (i + k) that stays an int for the element access; partway
// the sum leaves the int32 range, and later the base is negative.
function get(a, i, k) {
  return a[i + k] === undefined ? 'u' : a[i + k];
}
function sumAt(a, base, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += a[base + i] | 0;
  return s;
}
const arr = [];
for (let i = 0; i < 64; i++) arr.push(i * 3);
let w = 0;
for (let r = 0; r < 5000; r++) w += get(arr, r & 31, 7) + sumAt(arr, r & 15, 16);
console.log('warm', w);
const big = { length: 0 };
big[2147483648] = 'big';
big[-1] = 'neg';
console.log(get(big, 2147483647, 1), get(big, 2147483640, 8), get(arr, -3, 4), get(big, 0, -1), get(arr, 2147483647, 1));
console.log(sumAt(arr, 2147483640, 16), sumAt(arr, -8, 16), sumAt(arr, 60, 8));
console.log(sumAt(big, 2147483640, 16), big.length, arr.length);

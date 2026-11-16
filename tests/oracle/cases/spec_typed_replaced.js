// A closure's Float32Array read in a loop whose body calls out: partway the
// variable is replaced by a shorter Float32Array, then by an Array. The
// array, its kind and its length must be read again after each call.
let data = new Float32Array(30);
for (let i = 0; i < data.length; i++) data[i] = i * 0.5;
let swaps = 0;
function maybeSwap(i) {
  if (i === 5 && swaps === 0) { swaps++; data = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8, 9]); }
  if (i === 2 && swaps === 1) { swaps++; data = [10, 20, 30, 40, 50, 60]; }
}
function walk() {
  let s = 0;
  for (let i = 0; i < 10; i++) {
    const j = i * 3;
    s += (data[j] ?? -1) + (data[j + 1] ?? -2) + (data[j + 2] ?? -3);
    maybeSwap(i);
  }
  return s;
}
function main() {
  const out = [];
  for (let r = 0; r < 3; r++) out.push(walk());
  console.log(out.join(" "), data.length, swaps);
}
main();

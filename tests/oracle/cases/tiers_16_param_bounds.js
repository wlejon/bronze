function rangeSum(start, end, step, factor) {
  let acc = 0;
  let i = start;
  while (i < end) {
    acc = acc + i * factor;
    i = i + step;
  }
  return acc;
}
function run() {
  let r1 = rangeSum(0, 120000000, 2, 3);
  let r2 = rangeSum(10, 100000000, 5, 7);
  console.log(r1, r2);
}
run();

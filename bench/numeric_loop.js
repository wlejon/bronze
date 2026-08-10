// Straight-line float arithmetic inside a real loop; the loop-carried
// dependence keeps the work from being folded away.
function compute(n) {
  let x = 0.5;
  for (let i = 0; i < n; i++) {
    x = x * 1.000001 + 0.5;
    x = x * 0.999999 - 0.25;
    x = x * 1.000002;
  }
  return x;
}

console.log(compute(10000000));

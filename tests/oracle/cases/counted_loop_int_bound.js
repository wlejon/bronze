// Counted loops bounded by a function parameter or immutable outer binding
// stay int32 (`cmp.lt i32, i32`), eliminating overflow guards inside the
// loop while still leaving cleanly to G when passed negative, fractional,
// out-of-range int32, or non-finite bounds.
function sumParam(n) {
  let s = 0;
  for (let i = 0; i < n; i = i + 1) {
    s += i;
  }
  return s;
}

function makeClosedCounter(n) {
  return function () {
    let s = 0;
    for (let i = 0; i < n; i = i + 1) {
      s += i;
    }
    return s;
  };
}

function nearMax(n) {
  let hits = 0;
  for (let i = 2147483645; i < n; i = i + 1) {
    hits++;
  }
  return hits;
}

console.log(sumParam(0), sumParam(-5), sumParam(10), sumParam(1000));
console.log(sumParam(4.5), sumParam(NaN), sumParam(-Infinity));
console.log(makeClosedCounter(100)(), makeClosedCounter(3.5)());
console.log(nearMax(2147483647), nearMax(2147483649));

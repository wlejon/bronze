// Capture across two levels of function nesting.
function outer(a) {
  const middle = function (b) {
    const inner = function (c) {
      return a + b + c;
    };
    return inner(3);
  };
  return middle(2);
}
console.log(outer(1));

// `total` is shared by every closure the factory makes; `step` is private
// to each one.
function counterFactory() {
  let total = 0;
  return function (step) {
    return function () {
      total = total + step;
      return total;
    };
  };
}
const f = counterFactory();
const byTwo = f(2);
const byTen = f(10);
console.log(byTwo());
console.log(byTen());
console.log(byTwo());

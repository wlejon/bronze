// Function.prototype.toString (20.2.3.5)
function foo(a, b) {
  return a + b;
}

const bar = function () {};

console.log(foo.toString());
console.log(bar.toString());
console.log((function () {}).toString());
console.log(foo.call.toString());

try {
  foo.toString.call(123);
} catch (e) {
  console.log(e.name);
}

try {
  foo.toString.call(null);
} catch (e) {
  console.log(e.name);
}

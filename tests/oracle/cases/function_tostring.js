// Function.prototype.toString (20.2.3.5). A function with SOURCE TEXT returns
// that text verbatim; only a function without any — a builtin, a bound
// function — gets the NativeFunction string.
function foo(a, b) {
  return a + b;
}

const bar = function () {};

console.log(foo.toString());
console.log(bar.toString());
console.log((function () {}).toString());
console.log(Function.prototype.call.toString());

try {
  Function.prototype.toString.call(123);
} catch (e) {
  console.log(e.name);
}

try {
  Function.prototype.toString.call(null);
} catch (e) {
  console.log(e.name);
}

// A closure captures the variable, not a snapshot of its value, and each
// activation of the enclosing function gets its own binding.
function makeAdder(n) {
  return function (x) { return x + n; };
}
const add5 = makeAdder(5);
const add10 = makeAdder(10);
console.log(add5(3));
console.log(add10(3));
console.log(add5(add10(0)));

// Two closures over the SAME binding see each other's writes. The inner
// functions are declarations, which hoist within the enclosing body.
function pair() {
  let v = 1;
  function get() { return v; }
  function set(x) { v = x; }
  set(42);
  return get();
}
console.log(pair());

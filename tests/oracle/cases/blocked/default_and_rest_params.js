// Default parameter values and rest parameters. `function f(a, b = 2)` is
// `expected ')' after parameters` today, and `...rest` names nothing at all.
// A default is evaluated on every call that omits the argument, not once at
// definition, and it can see the parameters to its left. Rest is what a
// derived class with no constructor needs before it can stop being a named
// error (docs/0012 decision 5).
function add(a, b = 2) { return a + b; }
console.log(add(1));
console.log(add(1, 5));
function count(first, ...rest) { return first + ":" + rest.length; }
console.log(count(1));
console.log(count(1, 2, 3));
function later(a, b = a * 2) { return b; }
console.log(later(3));
const f = (x = "d") => x;
console.log(f());
console.log(f("v"));
let calls = 0;
function bump(v = ++calls) { return v; }
bump();
bump();
console.log(calls);
console.log(bump(9));
console.log(calls);

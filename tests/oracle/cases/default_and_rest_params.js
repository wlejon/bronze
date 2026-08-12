// Default parameter values and rest parameters.
//
// What each line pins: a default fires on an OMITTED argument and only on an
// omitted one; it is evaluated per call rather than once at definition, so
// `bump` leaves `calls` at exactly the number of calls that omitted `v`; it
// can read the parameters to its left (`later`); and it works the same in an
// arrow. A rest parameter is always a real array, empty rather than
// undefined when nothing is left over, whatever the caller passed.
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

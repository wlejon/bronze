// A top-level function declaration's binding is mutable: Babel's `_extends`
// helper replaces itself on its first call. A rebound declaration is still
// initialized before any statement runs, keeps its name, and a write inside
// its own body reaches the outer binding. A class declaration's outer binding
// is mutable too.
function Ni() {
  return (Ni = Object.assign ? Object.assign.bind() : function (t) { return t; }), Ni.apply(null, arguments);
}
function ext(a, b) { return Ni({}, a, b); }
console.log(JSON.stringify(ext({ a: 1 }, { b: 2 })), JSON.stringify(ext({ c: 3 }, {})));
console.log(early(), early.name, typeof early.prototype);
function early() { early = function () { return 'second'; }; return 'first'; }
console.log(early(), early.name);
function* gen() { yield 1; }
const firstGen = gen;
gen = null;
console.log(gen, [...firstGen()].length);
class Shape { static k() { return 'shape'; } }
function swapShape() { Shape = class { static k() { return 'other'; } }; }
console.log(Shape.k());
swapShape();
console.log(Shape.k());
function local() { return 'a'; }
local = () => 'b';
console.log(local());

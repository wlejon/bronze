// Babel's `_extends` helper (three's ktx-parse.module.js): a top-level
// function that replaces its own binding on first call. A function
// declaration's binding is mutable, so is an exported one (an importer sees
// the new value through the live binding), and so is a class declaration's
// outer binding.
function Ni() {
  return (Ni = Object.assign ? Object.assign.bind() : function (t) { return t; }), Ni.apply(null, arguments);
}
export function ext(a, b) { return Ni({}, a, b); }

export function counter() { return 1; }
export function bump() { counter = function () { return 2; }; }

class Shape { static k() { return 'shape'; } }
export function swapShape() { Shape = class { static k() { return 'other'; } }; }
export function shapeK() { return Shape.k(); }

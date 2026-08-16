// `Object.getPrototypeOf` / `setPrototypeOf` / `create` / `__proto__` over the
// receivers whose [[Prototype]] is not a field they carry.
//
// A plain object stores its prototype on its shape, so asking one is a load. An
// ARRAY and a FUNCTION store nothing: 23.1.6.1 and 20.2.3 fix their prototypes
// to %Array.prototype% and %Function.prototype% once and for all, so the answer
// is known without storage and `Object.getPrototypeOf([])` is `Array.prototype`
// exactly. That is also what makes the WRITE answerable: 10.1.2
// OrdinarySetPrototypeOf step 2 returns true when the requested prototype is
// already the current one, so `Object.setPrototypeOf(a, Array.prototype)` and
// `a.__proto__ = Array.prototype` are the no-ops the specification says they
// are rather than refusals — and the array keeps working afterwards, because
// nothing about it changed.
//
// %Function.prototype% is itself a function (20.2.3), and its own prototype is
// %Object.prototype% — not itself, which is what "a function's prototype is
// %Function.prototype%" would answer for the one function that IS the answer,
// and a chain walk over that would never end. %Object.prototype%'s own
// prototype is null (20.1.3), which is where every chain here terminates.
//
// A prototype write that would CHANGE an array's or a function's prototype has
// nowhere to be recorded and refuses by name, as does `getPrototypeOf` of an
// intrinsic bronze builds no prototype object for; neither is pinned here,
// because a refusal is not stdout.

const a = [1, 2];

console.log(Object.getPrototypeOf(a) === Array.prototype);
console.log(Object.getPrototypeOf(function () {}) === Function.prototype);
console.log(Object.getPrototypeOf(Array.prototype) === Object.prototype);
console.log(Object.getPrototypeOf(Function.prototype) === Object.prototype);
console.log(Object.getPrototypeOf(Object.prototype));
console.log(a.__proto__ === Array.prototype);

// The chain is the real one: a method found on it is the intrinsic's.
console.log(Object.getPrototypeOf(a).map === Array.prototype.map);
console.log(a.hasOwnProperty === Object.prototype.hasOwnProperty);

// 10.1.2 step 2: setting the prototype it already has changes nothing and
// answers with the object.
console.log(Object.setPrototypeOf(a, Array.prototype) === a);
a.__proto__ = Array.prototype;
console.log(a.length, a.map((x) => x * 2).join(","));

const f = function () {};
f.__proto__ = Function.prototype;
console.log(typeof f, Object.setPrototypeOf(f, Function.prototype) === f);

// `Object.create` builds a plain object over any prototype, including the
// exotic intrinsics and null.
console.log(Object.getPrototypeOf(Object.create(null)));
const overArray = Object.create(Array.prototype);
console.log(Object.getPrototypeOf(overArray) === Array.prototype);
console.log(Array.isArray(overArray), typeof overArray.map, typeof overArray.join);

const withProps = Object.create(Object.prototype, { x: { value: 4, enumerable: true } });
console.log(withProps.x, Object.keys(withProps).join(","));
console.log(Object.getPrototypeOf(withProps) === Object.prototype);

// A prototype walk terminates for each of them.
function depth(v) {
  let n = 0;
  for (let p = Object.getPrototypeOf(v); p !== null; p = Object.getPrototypeOf(p)) n++;
  return n;
}
console.log(depth([]), depth(function () {}), depth({}), depth(Object.create(null)));

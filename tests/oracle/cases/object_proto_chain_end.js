// The END of every prototype chain, reached from a receiver whose members
// bronze answers out of a C table rather than off a prototype object.
//
// 20.2.3 fixes `Function.prototype`'s [[Prototype]] at `Object.prototype`, and
// 23.1.3, 24.1.3, 24.2.3, 22.2.6, 23.2.3 and 10.4.3 do the same for the rest —
// so a function, an array, a Map, a Set, a RegExp, a typed array and a string
// all reach 20.1.3's six members. Reading one of them used to answer
// `undefined`, which is the silent fallback the house rules rank below a
// refusal, and it was gratuitous rather than pending: `f.toString` was already
// diagnosed by name from `Function.prototype`, one link NEARER, while
// `f.valueOf` one link further up read `undefined`.
//
// Each line pins which of three things a name gets.
//
//   The member ITSELF, and by identity — `a.hasOwnProperty` is the one function
//   object on `Object.prototype` rather than a per-receiver copy, which is what
//   makes this a chain rather than another table.
//
//   An own-property answer, for storage that is in no shape. 20.1.3.2 and
//   20.1.3.4 ask about the receiver alone, so an inherited member is false here
//   where `in` is true — `a.hasOwnProperty('push')` and `'push' in a` are two
//   questions and not two answers to one.
//
//   A NEARER prototype's member shadowing this one, which is why
//   `/ab/g.toString()` is 22.2.6.13's text and `new Map().toString()` is
//   20.1.3.6's tag.

function f(a, b) {}

// A function. 20.2.3.3's `call` is inherited from `Function.prototype`, while
// 10.2.10 and 10.2.9 make `length` and `name` own and non-enumerable.
console.log(typeof f.hasOwnProperty, 'hasOwnProperty' in f);
console.log(f.hasOwnProperty === Object.prototype.hasOwnProperty);
console.log(f.hasOwnProperty('length'), f.hasOwnProperty('name'), f.hasOwnProperty('call'));
console.log(f.propertyIsEnumerable('length'), f.propertyIsEnumerable('name'));
console.log(f.valueOf() === f);

// 20.1.3.3 walks the ARGUMENT's chain starting one link up, so nothing is its
// own prototype and `Object.prototype` is every chain's last link.
console.log(Object.prototype.isPrototypeOf(f), f.isPrototypeOf(f));

// 15.7.14 defines a static non-enumerable, and `extends` makes the base's
// statics inherited rather than own.
class C { static s() {} }
class D extends C {}
console.log(C.hasOwnProperty('s'), C.propertyIsEnumerable('s'));
console.log(typeof D.s, D.hasOwnProperty('s'), D.hasOwnProperty('name'));

// An array. Its own keys are its indices and 10.4.2.2's `length`, none of which
// lives in a shape; `push` is 23.1.3.23's and is inherited.
const a = [10, 20];
console.log(typeof a.hasOwnProperty, a.hasOwnProperty === Object.prototype.hasOwnProperty);
console.log(a.hasOwnProperty(0), a.hasOwnProperty(2), a.hasOwnProperty('length'),
            a.hasOwnProperty('push'));
console.log(a.propertyIsEnumerable(0), a.propertyIsEnumerable('length'));
console.log(a.valueOf() === a, 'valueOf' in a, Object.prototype.isPrototypeOf(a));

// A hole is not a key at all: `delete` takes the index out of the own keys and
// leaves `length` where it was.
const holed = [1, 2, 3];
delete holed[1];
console.log(holed.hasOwnProperty(1), holed.hasOwnProperty(2), holed.length);

// A Map and a Set. 24.1.3 and 24.2.3 give them no own property whatever —
// `size` is an accessor on the prototype and an entry is not a property at all
// — and neither prototype defines `toString`, so 20.1.3.6 answers, with the tag
// 24.1.3.13 and 24.2.3.12 put on those prototypes.
const m = new Map();
m.set('k', 1);
console.log(m.hasOwnProperty('k'), m.hasOwnProperty('size'), m.size);
console.log(m.toString(), m.valueOf() === m, 'hasOwnProperty' in m);
const st = new Set();
st.add(1);
console.log(st.hasOwnProperty('size'), st.toString(), Object.prototype.isPrototypeOf(st));

// A RegExp. 22.2.3.1 RegExpAlloc defines `lastIndex` as the one own property,
// non-enumerably; 22.2.6.10 makes `source` an accessor on the prototype however
// much bronze's header-backed answer looks like own data.
const re = /ab/g;
console.log(re.hasOwnProperty('lastIndex'), re.hasOwnProperty('source'),
            re.hasOwnProperty('global'));
console.log(re.propertyIsEnumerable('lastIndex'), re.toString(), re.valueOf() === re);

// A typed array. 10.4.5 makes an integer index within the length the only own
// property; `length` is 23.2.3.21's accessor and `BYTES_PER_ELEMENT` is
// 23.2.6.2's property of the prototype, so neither is own.
const ta = new Uint8Array(2);
ta[0] = 7;
console.log(ta.hasOwnProperty(0), ta.hasOwnProperty(5), ta.hasOwnProperty('length'),
            ta.hasOwnProperty('BYTES_PER_ELEMENT'));
console.log(ta.propertyIsEnumerable(0), ta.valueOf() === ta, 'hasOwnProperty' in ta);

// Primitives, where 20.1.3.2 step 2's ToObject would box. A String exotic
// object's own keys are 10.4.3.4's `length` and 10.4.3.5's one property per
// code unit and nothing else; 21.1, 20.3 and 20.4 give the other three wrappers
// no own property at all, so the answer is false whatever the key was.
console.log('ab'.hasOwnProperty(0), 'ab'.hasOwnProperty(2), 'ab'.hasOwnProperty('length'));
console.log('ab'.propertyIsEnumerable(0), 'ab'.propertyIsEnumerable('length'));
const sym = Symbol('t');
console.log(true.hasOwnProperty('x'), Object.prototype.hasOwnProperty.call(5, 'x'),
            sym.hasOwnProperty('description'));

// 20.1.3.3 step 1 is "if V is not an Object, return false", and an object made
// with a null prototype has no chain for the walk to find anything in.
console.log(Object.prototype.isPrototypeOf(5), Object.prototype.isPrototypeOf(Object.create(null)));

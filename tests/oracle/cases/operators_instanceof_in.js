// `instanceof` and `in` (docs/0015 decisions 5 and 6).
//
// `instanceof` walks the LEFT operand's prototype chain and compares each
// link against the right operand's `.prototype` (ECMA-262 7.3.22, with no
// Symbol.hasInstance because bronze has no symbols). A left operand that is
// not an object has no chain at all, so the answer is false rather than an
// error. `in` asks whether a key is reachable on an object - own properties
// AND inherited ones, which is what separates it from Object.keys.
function Point(x, y) {
  this.x = x;
  this.y = y;
}
Point.prototype.norm = 0;

const p = new Point(1, 2);
console.log(p instanceof Point);

class Animal {
  constructor(name) {
    this.name = name;
  }
}
class Dog extends Animal {
  constructor(name) {
    super(name);
    this.legs = 4;
  }
}
const d = new Dog("rex");
const a = new Animal("generic");
console.log(d instanceof Dog);
console.log(d instanceof Animal);
console.log(a instanceof Animal);
console.log(a instanceof Dog);
console.log(d instanceof Point);
console.log(p instanceof Animal);

// A non-object left operand has no prototype chain: false, never an error.
console.log(1 instanceof Point);
console.log("s" instanceof Point);
console.log(true instanceof Point);
console.log(null instanceof Point);
console.log(undefined instanceof Point);

// A plain object literal's prototype is not any constructor's.
console.log({} instanceof Point);

// `in` finds own keys and inherited ones alike.
console.log("x" in p);
console.log("y" in p);
console.log("norm" in p);
console.log("z" in p);
console.log("name" in d);
console.log("legs" in d);
console.log("legs" in a);

const o = { a: 1, b: undefined };
console.log("a" in o);
// A property whose VALUE is undefined still exists, which is exactly the
// question `in` answers and `o.b !== undefined` does not.
console.log("b" in o);
console.log(o.b);
console.log("c" in o);

// Array indices are keys, and so is `length`. An index past the end is not
// a key even though reading it is legal.
const arr = [10, 20, 30];
console.log(0 in arr);
console.log(2 in arr);
console.log(3 in arr);
console.log("length" in arr);
console.log("1" in arr);

// Precedence: both are relational operators, so they are looser than the
// arithmetic that builds their operands and tighter than equality.
console.log(("a" in o) === true);
console.log(1 + 1 in { "2": true });

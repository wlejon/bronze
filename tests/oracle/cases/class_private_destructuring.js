// Destructuring ASSIGNMENT onto a private member target — 13.15.5.5
// (array) and 13.15.5.6 (object), whose step 5 in each case is PutValue on a
// reference, and a reference to a private element is not a property reference.
//
// The whole point of the case: `({ a: this.#x } = v)` must perform the same
// write `this.#x = v.a` performs — brand check, accessor dispatch, method
// refusal — and must not put anything under the property name "#x", which is
// what a target lowered as an ordinary key would do and what no private read
// would ever see again.

const trace = [];

function* stepped() {
  trace.push("step");
  yield 41;
}

class Box {
  #x = 0;
  #y = 0;
  #tail = null;
  #seen = [];
  static #count = 0;

  get #logged() {
    return this.#seen.join("|");
  }
  set #logged(v) {
    this.#seen.push(v);
  }
  #frozen() {
    return "method";
  }
  get #readOnly() {
    return "ro";
  }

  objectPattern(src) {
    ({ a: this.#x, b: this.#y } = src);
    return this.#x + "," + this.#y;
  }
  withDefault(src) {
    // Only `undefined` fires a default, so `b` present-and-null keeps null.
    ({ a: this.#x = 5, b: this.#y = 6 } = src);
    return this.#x + "," + this.#y;
  }
  arrayPattern(src) {
    [this.#x, this.#y = 9] = src;
    return this.#x + "," + this.#y;
  }
  nested(src) {
    ({ p: { q: this.#x }, r: [, this.#y] } = src);
    return this.#x + "," + this.#y;
  }
  objectRest(src) {
    ({ a: this.#x, ...this.#tail } = src);
    return this.#x + "," + JSON.stringify(this.#tail);
  }
  arrayRest(src) {
    [this.#x, ...this.#tail] = src;
    return this.#x + "," + JSON.stringify(this.#tail);
  }
  // A setter target is CALLED, once per store, in the order the pattern
  // reaches it — the accessor half of 6.2.12.3 is not skipped by the pattern.
  accessorTarget(src) {
    ({ a: this.#logged } = src);
    [this.#logged] = ["second"];
    return this.#logged;
  }
  self() {
    trace.push("receiver");
    return this;
  }
  // The target's reference is evaluated before the iterator is stepped for it,
  // so the receiver's side effect precedes the generator's.
  ordering(src) {
    [this.self().#x] = src;
    return this.#x;
  }
  methodTarget(src) {
    ({ a: this.#frozen } = src);
  }
  readOnlyTarget(src) {
    [this.#readOnly] = src;
  }
  static counter(src) {
    ({ a: Box.#count } = src);
    return Box.#count;
  }
  static intruder(other, src) {
    [other.#x] = src;
  }
}

const b = new Box();
console.log(b.objectPattern({ a: 1, b: 2 }));
console.log(b.withDefault({ b: null }));
console.log(b.arrayPattern([3]));
console.log(b.nested({ p: { q: 7 }, r: [10, 11] }));
console.log(b.objectRest({ a: 12, m: 13, n: 14 }));
console.log(b.arrayRest([15, 16, 17]));
console.log(b.accessorTarget({ a: "first" }));
console.log(b.ordering(stepped()) + " " + trace.join(","));
console.log(Box.counter({ a: 99 }));

try {
  b.methodTarget({ a: 1 });
} catch (e) {
  console.log(e instanceof TypeError, e.message);
}
try {
  b.readOnlyTarget([1]);
} catch (e) {
  console.log(e instanceof TypeError, e.message);
}
try {
  Box.intruder({}, [1]);
} catch (e) {
  console.log(e instanceof TypeError, e.message);
}

// Nothing under a "#x" property name, and nothing visible at all.
console.log("[" + Object.getOwnPropertyNames(b).join(",") + "]", JSON.stringify(b));
console.log("#x" in b, Object.keys(b).length);

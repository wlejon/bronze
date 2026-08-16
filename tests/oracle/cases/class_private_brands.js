// Private-name IDENTITY, `#x in o`, static private elements, and static blocks.
//
// A private name belongs to one EVALUATION of a class, not to its source
// position (ECMA-262 15.7.14 creates a fresh PrivateEnvironment each time the
// definition is evaluated). So a class expression returned twice from a factory
// mints two sets of names, and an instance of the first fails the second's
// brand check even though both were written on the same line.
//
// `#x in o` (13.10.1) is how a program asks that question without a `try`: it
// answers a boolean for any OBJECT, and refuses a non-object at step 6.
//
// A STATIC private element is carried by the constructor itself, which is why
// `#n in SomeClass` is false for an instance field and `#made in SomeClass` is
// true for a static one, with no separate rule saying so.

function makeCounter() {
  return class Counter {
    #n = 0;
    static #made = 0;
    // Runs after the field above and before the ones below: 15.7.14 step 33
    // walks the element list ONCE, so blocks and static field initializers
    // interleave in definition order.
    static {
      Counter.#made = 100;
    }
    static trace = [];
    static {
      Counter.trace.push("block-1");
    }
    static seed = (Counter.trace.push("field"), 5);
    static {
      Counter.trace.push("block-2");
    }

    step() {
      return ++this.#n;
    }

    static holds(o) {
      return #n in o;
    }

    static hasStatic(o) {
      return #made in o;
    }

    static read(o) {
      return o.#n;
    }

    static madeCount() {
      return Counter.#made;
    }
  };
}

const A = makeCounter();
const B = makeCounter();
const a = new A();
const b = new B();

console.log(a.step(), a.step(), b.step());
// Each evaluation's brand recognizes only its own instances.
console.log(A.holds(a), A.holds(b), B.holds(b), B.holds(a));
console.log(A.holds({}), A.holds(A));
// The static element's brand is the constructor, and A's is not B's.
console.log(A.hasStatic(A), A.hasStatic(B), A.hasStatic(a));
console.log(A.madeCount(), B.madeCount());
console.log(A.trace.join(","));
console.log(B.trace.join(","));
console.log(A.trace === B.trace, A.seed, B.seed);

try {
  A.read(b);
} catch (e) {
  console.log(e.name + ": " + e.message);
}

try {
  A.holds(1);
} catch (e) {
  console.log(e.name + ": " + e.message);
}

// A static block's `this` is the constructor (15.7.11), and it runs before
// anything outside the class body can observe the class.
class WithThis {
  static #v = "unset";
  static {
    this.#v = "set-by-block";
    this.viaThis = this === WithThis;
  }

  static value() {
    return WithThis.#v;
  }
}

console.log(WithThis.value(), WithThis.viaThis);

// A nested class shadows a name it repeats and reaches one it does not:
// ResolvePrivateIdentifier walks the private environments innermost first.
class Outer {
  #tag = "outer";
  probe() {
    class Inner {
      #own = "inner";
      both(other, outer) {
        return [other.#own, outer.#tag].join("/");
      }
    }
    return new Inner().both(new Inner(), this);
  }
}

console.log(new Outer().probe());

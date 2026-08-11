// `Foo.prototype.constructor` — the back-pointer ECMA-262 10.2.5
// (MakeConstructor) step 6 installs on every constructor's prototype object,
// with the attributes it names: writable, NOT enumerable, configurable.
// 15.7.14 (ClassDefinitionEvaluation) reaches the same step for a class.
//
// It exists here because `new this.constructor(...)` is how prototype-style
// code clones an object without naming its class, and three.js does exactly
// that in Box3, BufferAttribute and AnimationUtils. Before this the
// expression was `undefined is not a constructor`.

class Vec {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
  // The polymorphic clone: it never names `Vec`, so a subclass inherits it
  // and gets an instance of ITSELF back. That is the whole reason the
  // property is worth having.
  clone() {
    return new this.constructor(this.x, this.y);
  }
  add(o) {
    return new this.constructor(this.x + o.x, this.y + o.y);
  }
}

class Tagged extends Vec {
  constructor(x, y) {
    super(x, y);
    this.tag = "tagged";
  }
}

const v = new Vec(1, 2);
const c = v.clone();
console.log(c.x);
console.log(c.y);
console.log(c === v);

const t = new Tagged(3, 4);
const tc = t.clone();
console.log(tc.x);
console.log(tc.tag);
console.log(t.add(v).tag);

// 10.2.5 step 6 puts the property on the constructor's OWN prototype object,
// and 15.7.14 gives a derived class its own, so the two do not share one.
console.log(Vec.prototype.constructor === Vec);
console.log(Tagged.prototype.constructor === Tagged);
console.log(Tagged.prototype.constructor === Vec);

// Not enumerable: 10.2.5 step 6 passes { [[Enumerable]]: false }, so
// Object.keys (20.1.2.17, own enumerable string keys) does not report it, and
// neither does a for-in (14.7.5.1 skips non-enumerable properties on every
// link of the chain). The class's methods are non-enumerable for the same
// reason (15.7.14), so the prototype has no enumerable own keys at all.
console.log(Object.keys(Vec.prototype).length);
const seen = [];
for (const k in t) seen.push(k);
console.log(seen.join(","));

// An ordinary function is a constructor too (10.2.5 runs for any function
// with a [[Construct]]), so the same back-pointer is there without `class`.
function Node(label) {
  this.label = label;
}
console.log(Node.prototype.constructor === Node);

// The three.js AnimationUtils shape: reach the class through an INSTANCE and
// build a fresh one. `values.constructor` is an inherited read that lands on
// the prototype's back-pointer.
const sample = new Node("first");
const fresh = new sample.constructor("second");
console.log(fresh.label);
console.log(sample.constructor === Node);

// Assigning a new `.prototype` hands instances the object the program
// supplied: 10.2.2 OrdinaryCreateFromConstructor reads `.prototype` at
// construction time, and 10.2.5's back-pointer was installed on the object
// that has been replaced, not on the function.
function Reset() {}
Reset.prototype = { marker: "replaced" };
console.log(new Reset().marker);

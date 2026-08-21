// Value flow through a MODULE-SCOPE binding: what a name the module declares
// once and the whole program shares is observed to hold, joined over every
// declaration and assignment anywhere.
//
// No scope chain connects the two ends of that — a module-level function's
// scope has no parent, and the module top level is a separate body — so the
// join is a table keyed by name, and a table keyed by name is exactly what a
// shadowing declaration, a temporal dead zone and a hoisted `var` can be read
// through. Every line below is one of those roads, and each has to answer what
// it answered before the table existed.

class V {
  constructor(x) {
    this.x = x;
    this.y = 0;
  }
  bump() {
    this.x = this.x + 1;
    return this;
  }
}

class W {
  constructor() {
    this.x = "w";
  }
}

// The case the table exists for: one module const holding a `new`, read from a
// function whose scope chain does not reach the module top level, and from a
// class method, which is a second body again.
const shared = new V(10);

function readShared() {
  return shared.x + "," + shared.y;
}

class Reader {
  read() {
    return shared.x;
  }
}

console.log(readShared());
console.log(new Reader().read());

// The same binding, mutated from inside a function. The identity is unchanged
// and the value is not the one the declaration wrote.
function bumpShared() {
  shared.bump();
}
bumpShared();
console.log(readShared());

// A module `let` that holds two different classes over its life. The join keeps
// the kind and loses the identity, and both reads answer for the object that
// was actually there.
let swapped = new V(1);
function readSwapped() {
  return swapped.x;
}
console.log(readSwapped());
swapped = new W();
console.log(readSwapped());

// A read that reaches the binding before the declaration ran. `const` is in its
// temporal dead zone until the statement initializes it (ECMA-262 9.1.1.1.6),
// and a table that answered for it would have to be wrong about the value while
// being right about the identity.
function earlyRead() {
  return later.x;
}
let earlyResult;
try {
  earlyResult = earlyRead();
} catch (e) {
  earlyResult = e instanceof ReferenceError ? "tdz" : "wrong:" + e.name;
}
const later = new V(99);
console.log(earlyResult + "," + earlyRead());

// A function-scoped `var` with the same name as a module binding. Inside the
// function the name is the VAR — hoisted, so `undefined` before its statement
// runs and never the module's object.
const collide = new V(5);
function shadowVar() {
  const before = typeof collide;
  var collide = new W();
  return before + "," + collide.x;
}
console.log(shadowVar() + "," + collide.x);

// A parameter with the same name shadows the module binding for the whole body.
function shadowParam(shared) {
  return shared.x;
}
console.log(shadowParam(new W()));

// A block-scoped declaration shadowing it, and the module binding visible again
// after the block.
function shadowBlock() {
  let out = "";
  {
    const shared = new W();
    out += shared.x;
  }
  out += "/" + shared.x;
  return out;
}
console.log(shadowBlock());

// A module const holding an object LITERAL — an identity with no constructor
// name — read through a function, then grown a property, which puts it at a
// new shape.
const bag = { a: 1, b: 2 };
function readBag() {
  return bag.a + bag.b;
}
console.log(readBag());
bag.c = 3;
console.log(readBag() + "," + bag.c);

// A module const holding a PRIMITIVE. Nothing about it may be believed as a
// value: the table has no program order, so it cannot say the read happens
// after the write.
const count = 7;
function readCount() {
  return count * 2;
}
console.log(readCount());

// A module binding first assigned inside a function that runs before anything
// reads it. The write is not in the top-level body at all.
let deferred;
function initDeferred() {
  deferred = new V(3);
}
function useDeferred() {
  return deferred.x;
}
initDeferred();
console.log(useDeferred());

// Warm past the point where every cache and guard on the path is filled: the
// module binding is the receiver at a hot site, which is the shape three.js's
// shared temporaries have.
const hotVec = new V(0);
let total = 0;
for (let i = 0; i < 500; i++) {
  hotVec.bump();
  total += hotVec.x;
}
console.log(hotVec.x + "," + total);

// The binding reassigned to a NON-object after the hot loop. Every read after
// it sees the new value.
let mutable = new V(1);
function readMutable() {
  return typeof mutable;
}
console.log(readMutable());
mutable = "text";
console.log(readMutable() + "," + mutable.length);

// Class fields are DEFINED (7.3.33 DefineField: CreateDataPropertyOrThrow),
// for every spelling and wherever the interfering property lives: on the
// class's own prototype, a base class's, Object.prototype, or — for a static
// field — a base constructor. A definition refuses only what the RECEIVER
// itself forbids: a non-configurable own property, a non-extensible object.

const log = [];
function flush() {
  const s = log.join(",");
  log.length = 0;
  return s;
}
function own(o, k) {
  const d = Object.getOwnPropertyDescriptor(o, k);
  if (d === undefined) return String(k) + ":absent";
  if ("get" in d) return String(k) + ":accessor";
  return String(k) + ":" + String(d.value) + (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") +
    (d.configurable ? "c" : "-");
}

// A setter and a read-only property put on a class's prototype by hand,
// after the class body: nothing in the body says they are there.
class A {
  a = 1;
  b = 2;
  ["c"] = 3;
}
Object.defineProperty(A.prototype, "a", {
  set(v) {
    log.push("A.prototype set a " + v);
  },
  configurable: true,
});
Object.defineProperty(A.prototype, "b", { value: "proto b", writable: false });
Object.defineProperty(A.prototype, "c", {
  set(v) {
    log.push("A.prototype set c " + v);
  },
});
const a1 = new A();
console.log("own prototype", flush(), own(a1, "a"), own(a1, "b"), own(a1, "c"));

// The same on a base class's prototype, reached through `extends`.
class B extends A {
  a = 4;
  b = 5;
}
const b1 = new B();
console.log("base prototype", flush(), own(b1, "a"), own(b1, "b"), own(b1, "c"));

// Object.prototype, for a field name, a computed one and an integer-like one.
Object.defineProperty(Object.prototype, "op", {
  set(v) {
    log.push("Object.prototype set op " + v);
  },
  configurable: true,
});
Object.defineProperty(Object.prototype, "ro", { value: "inherited", writable: false, configurable: true });
const key = "op";
class C {
  op = "named";
  ro = "read-only";
  [key + "2"] = "computed";
}
Object.defineProperty(Object.prototype, "op2", {
  set(v) {
    log.push("Object.prototype set op2 " + v);
  },
  configurable: true,
});
const c1 = new C();
console.log("Object.prototype", flush(), own(c1, "op"), own(c1, "ro"), own(c1, "op2"));

// Static fields define on the constructor: a base constructor's static
// setter is not run.
class SBase {
  static set s(v) {
    log.push("SBase set s " + v);
  }
}
class S extends SBase {
  static s = "static";
  static ["t"] = "static computed";
  static op = "static op";
}
console.log("static", flush(), own(S, "s"), own(S, "t"), own(S, "op"));

// An anonymous function a field holds takes the field's name.
class N {
  fn = function () {};
  arrow = () => 0;
  static sfn = function () {};
}
console.log("names", new N().fn.name, new N().arrow.name, N.sfn.name);

// The receiver's OWN property decides. A configurable accessor a base
// constructor put on `this` is replaced by the data property...
class OwnAccessor {
  constructor() {
    Object.defineProperty(this, "x", {
      get() { return "base getter"; },
      set(v) { log.push("own setter " + v); },
      configurable: true,
    });
  }
}
class ReplacesAccessor extends OwnAccessor {
  x = "field";
}
const r1 = new ReplacesAccessor();
console.log("replaces accessor", flush(), own(r1, "x"));

// ...a non-configurable one refuses, with a TypeError...
class Pinned {
  constructor() {
    Object.defineProperty(this, "x", { value: "pinned", writable: true, enumerable: true });
  }
}
class OverPinned extends Pinned {
  x = "field";
}
try {
  new OverPinned();
  console.log("non-configurable stored");
} catch (e) {
  console.log("non-configurable", e instanceof TypeError);
}

// ...and so does an object the base returned frozen.
class ReturnsFrozen {
  constructor() {
    return Object.freeze({ kept: 1 });
  }
}
class OntoFrozen extends ReturnsFrozen {
  y = "field";
}
try {
  new OntoFrozen();
  console.log("non-extensible stored");
} catch (e) {
  console.log("non-extensible", e instanceof TypeError);
}

// A field after a constructor assignment of the same name in the base: the
// own property exists, writable and configurable, and the field overwrites it.
class AssignsFirst {
  constructor() {
    this.z = "base";
  }
}
class Overwrites extends AssignsFirst {
  z = "field";
}
console.log("overwrite", own(new Overwrites(), "z"));

delete Object.prototype.op;
delete Object.prototype.op2;
delete Object.prototype.ro;
console.log("restored", ({}).op, ({}).ro);

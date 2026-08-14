// new.target meta-property in functions, classes, and inheritance.

function PlainFunc() {
  if (new.target) {
    console.log("PlainFunc constructed with:", new.target === PlainFunc);
  } else {
    console.log("PlainFunc called without new:", new.target === undefined);
  }
}

PlainFunc();
new PlainFunc();

class Base {
  constructor() {
    console.log("Base ctor new.target is Base:", new.target === Base);
    console.log("Base ctor new.target is Derived:", new.target === Derived);
  }
}

class Derived extends Base {
  constructor() {
    super();
    console.log("Derived ctor new.target is Derived:", new.target === Derived);
  }
}

console.log("--- new Base ---");
new Base();

console.log("--- new Derived ---");
new Derived();

function Factory(kind) {
  if (!new.target) {
    return new Factory(kind);
  }
  this.kind = kind;
  console.log("Constructed instance with kind:", this.kind);
}

const f1 = new Factory("explicit");
const f2 = Factory("implicit");
console.log(f1.kind, f2.kind);

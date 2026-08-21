// ECMA-262 28.1.2 Reflect.construct ( target, argumentsList [ , newTarget ] )

function A(x, y) {
  this.x = x;
  this.y = y;
}

class B {
  constructor(name) {
    this.name = name;
  }
}

class DerivedB extends B {
  constructor(name, extra) {
    super(name);
    this.extra = extra;
  }
}

// Basic construction
const a = Reflect.construct(A, [10, 20]);
console.log(a instanceof A, a.x, a.y);

const b = Reflect.construct(B, ["hello"]);
console.log(b instanceof B, b.name);

const d = Reflect.construct(DerivedB, ["world", 42]);
console.log(d instanceof DerivedB, d instanceof B, d.name, d.extra);

// Construction with newTarget
function Base() {
  this.kind = "base";
}
function Sub() {}
Sub.prototype = { custom: true };

const customInst = Reflect.construct(Base, [], Sub);
console.log(customInst.kind, customInst.custom, Object.getPrototypeOf(customInst) === Sub.prototype);

// Builtin construction
const arr = Reflect.construct(Array, [3]);
console.log(Array.isArray(arr), arr.length);

// Error cases
function catchErr(fn) {
  try {
    fn();
    return "no-throw";
  } catch (e) {
    return e instanceof TypeError ? "TypeError" : e.constructor.name;
  }
}

console.log(catchErr(() => Reflect.construct(123, [])));
console.log(catchErr(() => Reflect.construct(A, 123)));
console.log(catchErr(() => Reflect.construct(A, [], 123)));
console.log(catchErr(() => Reflect.construct(() => {}, [])));
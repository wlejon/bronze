// Tests for super property access and calls inside object-literal methods,
// computed super access in classes and object literals, and dynamic prototype walk.

const protoA = {
  name: "protoA",
  greet(suffix) {
    return "hello from " + this.name + suffix;
  },
  val: 10,
  get computedVal() {
    return this.val * 2;
  },
  set setterVal(v) {
    this.saved = v * 3;
  },
};

const obj = {
  name: "obj",
  val: 20,
  // 1. Shorthand method super.prop and super.m(...)
  callSuperMethod() {
    return super.greet("!");
  },
  readSuperVal() {
    return super.val;
  },
  // 2. Computed super[expr] and super[expr](...)
  readSuperComputed(key) {
    return super[key];
  },
  callSuperComputed(key, arg) {
    return super[key](arg);
  },
  // 3. super property assignment (named and computed)
  writeSuperProp(v) {
    super.written = v;
  },
  writeSuperComputed(key, v) {
    super[key] = v;
  },
  // 4. Compound assignment
  bumpSuperProp() {
    super.counter = (super.counter || 0) + 1;
  },
  bumpSuperCompound() {
    super.c2 = 5;
    super.c2 += 10;
  },
  // 5. Accessors
  get superGetter() {
    return super.computedVal;
  },
  set superSetter(v) {
    super.setterVal = v;
  },
  // 6. Nested arrow function captures home object from enclosing method
  arrowSuper() {
    const fn = () => super.greet(" from arrow");
    return fn();
  },
  arrowSuperComputed(k) {
    const fn = () => super[k];
    return fn();
  },
};

Object.setPrototypeOf(obj, protoA);

console.log(obj.callSuperMethod());
console.log(obj.readSuperVal());
console.log(obj.readSuperComputed("val"));
console.log(obj.callSuperComputed("greet", " via computed"));
console.log(obj.superGetter);

obj.superSetter = 7;
console.log(obj.saved, protoA.saved === undefined);

obj.writeSuperProp(42);
console.log(obj.written, protoA.written === undefined);

obj.writeSuperComputed("compKey", 99);
console.log(obj.compKey, protoA.compKey === undefined);

obj.bumpSuperProp();
obj.bumpSuperProp();
console.log(obj.counter);

obj.bumpSuperCompound();
console.log(obj.c2);

console.log(obj.arrowSuper());
console.log(obj.arrowSuperComputed("name"));

// 7. Dynamic prototype change on the home object
const protoB = {
  name: "protoB",
  greet(suffix) {
    return "greetings from " + this.name + suffix;
  },
  val: 50,
  get computedVal() {
    return this.val * 4;
  },
};

Object.setPrototypeOf(obj, protoB);
console.log(obj.callSuperMethod());
console.log(obj.readSuperVal());
console.log(obj.superGetter);

// 8. Class methods with computed super[expr] and computed assignment
class BaseClass {
  constructor() {
    this.fromBase = 100;
  }
  methodA(x) {
    return "Base.methodA:" + x;
  }
}

class DerivedClass extends BaseClass {
  readComputed(key) {
    return super[key];
  }
  callComputed(key, arg) {
    return super[key](arg);
  }
  writeComputed(key, val) {
    super[key] = val;
  }
  compoundComputed(key, delta) {
    super[key] = (super[key] || 0) + delta;
  }
}

const derived = new DerivedClass();
console.log(derived.callComputed("methodA", 42));
derived.writeComputed("extra", 55);
console.log(derived.extra, BaseClass.prototype.extra === undefined);
derived.compoundComputed("accum", 10);
derived.compoundComputed("accum", 20);
console.log(derived.accum);

// 9. Async and generator methods in object literal
const asyncGenObj = {
  baseVal: 1,
  *genMethod() {
    yield super.genBase;
    yield super["genBase"] + 1;
  },
  async asyncMethod() {
    return super.asyncBase;
  },
};
const asyncGenProto = {
  genBase: 7,
  asyncBase: "resolved-async-super",
};
Object.setPrototypeOf(asyncGenObj, asyncGenProto);

const gen = asyncGenObj.genMethod();
console.log(gen.next().value);
console.log(gen.next().value);

asyncGenObj.asyncMethod().then((val) => {
  console.log(val);
});

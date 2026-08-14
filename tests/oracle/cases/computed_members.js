// Computed members in classes and object literals: methods, accessors, and generators.

const mKey = "myMethod";
const sKey = "myStaticMethod";
const gKey = "myGetter";
const setKey = "mySetter";
const genKey = "myGen";
const sym = Symbol("customMethod");

class AdvancedClass {
  _val = 10;

  [mKey](arg) {
    return "instance method called with: " + arg;
  }

  static [sKey](arg) {
    return "static method called with: " + arg;
  }

  get [gKey]() {
    return this._val * 2;
  }

  set [setKey](v) {
    this._val = v;
  }

  *[genKey]() {
    yield this._val;
    yield this._val + 1;
  }

  [sym]() {
    return "symbol method result";
  }
}

console.log("--- Class computed members ---");
console.log(AdvancedClass[sKey]("staticArg"));

const inst = new AdvancedClass();
console.log(inst[mKey]("instArg"));
console.log("Getter:", inst[gKey]);
inst[setKey] = 50;
console.log("Getter after set:", inst[gKey]);

const genInst = inst[genKey]();
console.log("Gen 1:", genInst.next().value);
console.log("Gen 2:", genInst.next().value);
console.log("Gen done:", genInst.next().done);
console.log("Sym method:", inst[sym]());

console.log("--- Object literal computed members ---");
const obj = {
  _inner: 100,
  [mKey](x) {
    return "obj method: " + x;
  },
  get [gKey]() {
    return this._inner + 5;
  },
  set [setKey](v) {
    this._inner = v;
  },
  *[genKey]() {
    yield this._inner;
    yield this._inner * 2;
  }
};

console.log(obj[mKey]("objArg"));
console.log("Obj getter:", obj[gKey]);
obj[setKey] = 200;
console.log("Obj getter after set:", obj[gKey]);

const objGen = obj[genKey]();
console.log("Obj Gen 1:", objGen.next().value);
console.log("Obj Gen 2:", objGen.next().value);
console.log("Obj Gen done:", objGen.next().done);

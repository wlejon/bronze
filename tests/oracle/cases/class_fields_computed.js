// Computed class fields: instance and static computed field keys.

let evalOrder = [];

const k1 = "inst_" + (evalOrder.push("k1"), 1);
const k2 = "static_" + (evalOrder.push("k2"), 2);
const sym = Symbol("custom");

class DynamicFields {
  [k1] = "instance value 1";
  static [k2] = "static value 2";
  [sym] = "symbol field value";
  static [sym] = "static symbol field value";
  ["litKey"] = 100;
  static ["staticLitKey"] = 200;

  constructor() {
    console.log("Ctor constructed");
  }
}

console.log("Eval order:", evalOrder.join(","));
console.log("Static k2:", DynamicFields[k2]);
console.log("Static sym:", DynamicFields[sym]);
console.log("Static lit:", DynamicFields.staticLitKey);

const inst = new DynamicFields();
console.log("Inst k1:", inst[k1]);
console.log("Inst sym:", inst[sym]);
console.log("Inst lit:", inst.litKey);

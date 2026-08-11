// Own-key order is spec'd JS order, recovered from the shape transition
// chain (docs/0009): integer-like keys ascending numerically FIRST, then
// every other string key in insertion order.
//
// `printKeys` walks the result rather than printing the array, because an
// array's console.log format is not pinned yet — the order is what is
// under test, and it prints one key per line.
function printKeys(o) {
  const k = Object.keys(o);
  console.log(k.length);
  let i = 0;
  while (i < k.length) {
    console.log(k[i]);
    i = i + 1;
  }
}

// Inserted b, "2", a, "10", "1". String order would put "10" before "2";
// numeric order is what JS specifies.
const mixed = {};
mixed.b = 1;
mixed["2"] = 2;
mixed.a = 3;
mixed["10"] = 4;
mixed["1"] = 5;
printKeys(mixed);

// Insertion order survives reassignment: writing an existing property is
// not a new transition, so `first` stays first.
const reassigned = {};
reassigned.first = 1;
reassigned.second = 2;
reassigned.first = 99;
printKeys(reassigned);

// Keys that only LOOK like integers are ordinary string keys, and stay in
// insertion order behind the real ones: leading zero, negative, decimal
// point, and one past the last array index (2^32-1).
const notIntegers = {};
notIntegers["01"] = 1;
notIntegers["7"] = 2;
notIntegers["-1"] = 3;
notIntegers["1.0"] = 4;
notIntegers["4294967295"] = 5;
notIntegers["0"] = 6;
printKeys(notIntegers);

// An object literal's keys are in source order.
printKeys({ z: 1, y: 2, x: 3 });

// Own keys only: nothing from the prototype chain appears.
function Thing() {
  this.own = 1;
}
Thing.prototype.inherited = 2;
printKeys(new Thing());

// An empty object has no keys, and that is not an error.
printKeys({});

// An array's own keys are its indices, as strings, ascending.
printKeys(["p", "q", "r"]);

// 7.3.19 CreateListFromArrayLike, through the two members that call it:
// `Function.prototype.apply` (20.2.3.1 step 4) and `Reflect.apply` (28.1.1
// step 2). An argument list is not required to be an Array — it is any object
// with a `length` and indexed reads, which is what `arguments`, a DOM NodeList
// and a hand-written `{length: 2, 0: "a", 1: "b"}` all are.
//
// The length goes through ToLength (7.3.18), so a string "2" is two arguments,
// a negative or a fraction below one is none, and 2.9 truncates to 2. An index
// the object does not carry reads as `undefined` rather than being skipped:
// 7.3.19 step 5 is Get, which has no notion of a hole, and the holey line below
// pins the consequence — `Math.max` over a hole is NaN, not the max of the rest.
//
// The two members differ on ONE step and it is pinned: 20.2.3.1 step 3 makes
// `null` and `undefined` mean "no arguments", while 28.1.1 has no such step and
// puts them straight into CreateListFromArrayLike, whose step 1 is a TypeError.
// A PRIMITIVE argument list is that same TypeError for both, and a catchable
// one — the specification says exactly what the call means, so refusing it as
// unimplemented would have been the wrong shape of error.
//
// `Function.prototype.call` takes its arguments positionally and never reaches
// 7.3.19 at all, which the last line pins by passing it an object that WOULD
// have been an argument list to `apply`.
function report() {
  var out = "[" + arguments.length + "]";
  for (var i = 0; i < arguments.length; i++) {
    out += (i ? "|" : "") + String(arguments[i]);
  }
  return out;
}

console.log(Math.max.apply(null, { length: 3, 0: 1, 1: 9, 2: 4 }));
console.log(Math.max.apply(null, { length: 3, 0: 1, 2: 4 }));

console.log(report.apply(null, { length: 2, 0: "a", 1: "b" }));
console.log(report.apply(null, { length: "2", 0: "a", 1: "b" }));
console.log(report.apply(null, { length: 2.9, 0: "a", 1: "b", 2: "c" }));
console.log(report.apply(null, { length: -1, 0: "a" }));
console.log(report.apply(null, {}));
console.log(report.apply(null, [1, , 3]));
console.log(report.apply(null, []));
console.log(report.apply(null, null), report.apply(null, undefined));

try {
  report.apply(null, 5);
} catch (e) {
  console.log("apply primitive:", e instanceof TypeError);
}

console.log(Reflect.apply(Math.max, null, { length: 2, 0: 1, 1: 5 }));
console.log(Reflect.apply(report, null, { length: 2, 0: "x", 1: "y" }));
console.log(Reflect.apply(report, null, [7, 8]));
try {
  Reflect.apply(report, null, null);
} catch (e) {
  console.log("Reflect.apply null:", e instanceof TypeError);
}
try {
  Reflect.apply(report, null, 5);
} catch (e) {
  console.log("Reflect.apply primitive:", e instanceof TypeError);
}

console.log(report.call(null, { length: 2, 0: "a", 1: "b" }));

// The `this` an array-like call receives is still the first argument, not
// anything the list carries.
function whoAmI() {
  return this.tag + "/" + arguments.length;
}
console.log(whoAmI.apply({ tag: "here" }, { length: 2, 0: 0, 1: 0 }));

// `typeof` on a name nothing in the program declares.
//
// From ECMA-262 13.5.3 (the typeof operator):
//
// 1. Step 1: "If val is a Reference Record and IsUnresolvableReference(val)
//    is true, return "undefined"." So `typeof __BRONZE_DEVTOOLS__` is the
//    STRING "undefined" and raises nothing — this is the feature-detection
//    idiom, and it is legal in both directions of the comparison.
// 2. The exemption is for an unresolvable REFERENCE. It does not extend to a
//    resolvable one, so `typeof` of a declared binding is the type of its
//    value, and it does not extend to a member expression either — that case
//    is in unresolved_reference.js, because evaluating it throws.
// 3. Table 41: the six results. `typeof null` is "object" (the historical
//    result the table pins), `typeof undefined` is "undefined", and a
//    namespace object is an ordinary object.

if (typeof __BRONZE_DEVTOOLS__ !== "undefined") {
  console.log("present");
} else {
  console.log("absent");
}

if (typeof __BRONZE_DEVTOOLS__ === "undefined") {
  console.log("absent again");
} else {
  console.log("present again");
}

console.log(typeof __BRONZE_DEVTOOLS__);
console.log(typeof __BRONZE_DEVTOOLS__ === typeof undefined);

// A second unresolvable name, so the answer is not about the first one.
console.log(typeof document, typeof window, typeof fetch);

// Resolvable names, for contrast: the exemption changes nothing about these.
const declared = 1;
function fn() {}
console.log(typeof declared, typeof fn, typeof Math, typeof Math.sqrt);
console.log(typeof undefined, typeof null, typeof "s", typeof true, typeof {});

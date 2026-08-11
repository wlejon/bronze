// A bracket key names an array ELEMENT only when it is a canonical array
// index: the decimal form must round-trip (docs/0009 decision 1). `"1x"`,
// `"01"` and `"1.5"` are ordinary named properties, which an array does not
// carry, so each reads `undefined` rather than element 1 — the answer a
// leading-digits parse would give. The numeric literal forms ask the same
// question from the other side: `a[1.5]` is the property `"1.5"`, not
// element 1, and `a[2]` is element 2 because `2` round-trips.
// Every expectation is ECMA-262, derived by hand (docs/0003).
const a = [10, 20, 30];
console.log(a[1]);
console.log(a["1"]);
console.log(a[1.5]);
console.log(a["1x"]);
console.log(a["01"]);
console.log(a["1.5"]);
console.log(a[" 1"]);
console.log(a["-1"]);
console.log(a[""]);
console.log(a["4294967295"]);
console.log(a.length);

// The same keys on a plain object, where every one of them IS a property:
// an object stores them by name, so the ones an array refuses are the ones
// an object answers.
const o = { "1": "one", "1.5": "onepointfive", "01": "ohone", "1x": "onex" };
console.log(o[1]);
console.log(o["1"]);
console.log(o["1.5"]);
console.log(o["01"]);
console.log(o["1x"]);
console.log(o["2"]);

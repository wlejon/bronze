// What `JSON.stringify` does with an object that is not a plain one. Two
// different right answers live here and `{}` is well-formed JSON for both, so
// a wrong one is invisible in the output — which is how a typed array came to
// serialize as `{}` while `Object.entries` on the same object was correct.
//
// 25.5.2.4 asks for EnumerableOwnPropertyNames. A typed array HAS them:
// 10.4.5.3 makes every integer-indexed element an own enumerable property, so
// the keys are "0", "1", ... and the answer is an object, not an array — the
// exotic kind is not an Array, so 25.5.2.1 step 4's IsArray test is false.
// A Map, a Set, a RegExp, an ArrayBuffer and a DataView keep everything in
// internal slots and genuinely have none, so `{}` is the real answer for them
// and the reason `JSON.stringify(map)` surprises everyone in every engine.
//
// Every byte below was derived from the specification before bronze was run.

console.log(JSON.stringify(new Uint8Array([1, 2, 3])));
console.log(JSON.stringify(new Int16Array([-7])));
// No elements, so no own enumerable keys — `{}` here is the same answer a Map
// gets, arrived at from the opposite direction.
console.log(JSON.stringify(new Float64Array(0)));

// 25.5.2.4 recurses through SerializeJSONProperty, so a view nested in a plain
// object is serialized by the same walk rather than by a special case.
console.log(JSON.stringify({ t: new Int16Array([7]), n: 1 }));

// A non-finite number is `null` (25.5.2.1 step 10), and a typed array's
// elements reach that step like any other number.
console.log(JSON.stringify(new Float64Array([NaN, Infinity, -Infinity, -0])));

// The `space` argument indents these members like any others: the keys are
// ordinary own keys once the walk has them.
console.log(JSON.stringify(new Uint8Array([1, 2]), null, 2));

// 7.1.11 ToUint8Clamp rounds half to EVEN, so 1.5 clamps to 2 and 2.5 also
// clamps to 2 — the stored bytes are what serialize, not the arguments.
console.log(JSON.stringify(new Uint8ClampedArray([1.5, 2.5, 300, -5])));

// The kinds with nothing own and enumerable to report.
console.log(JSON.stringify(new Map([['a', 1]])));
console.log(JSON.stringify(new Set([1, 2])));
console.log(JSON.stringify(/ab+c/gi));
console.log(JSON.stringify(new ArrayBuffer(4)));
console.log(JSON.stringify(new DataView(new ArrayBuffer(4))));

// An array CONTAINING them: 25.5.2.5 writes every element, so the `{}` answers
// appear as members rather than being omitted the way `undefined` would be.
console.log(JSON.stringify([new Map(), new Uint8Array([9])]));

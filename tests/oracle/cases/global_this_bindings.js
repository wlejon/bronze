// 9.1.1.4: the global environment's OBJECT RECORD is `globalThis`, so a
// property of that object is a global binding and the two spellings are one
// thing. `globalThis.x = 1` therefore creates a binding a later bare `x`
// reads — which no compile-time pass can see, and which bronze used to
// resolve to nothing at all.
//
// 19.1 also puts three VALUE properties on it, all non-writable and
// non-configurable.
console.log(globalThis.Infinity, globalThis.NaN, globalThis.undefined);
console.log(globalThis.Infinity === Infinity, globalThis.undefined === undefined);
const d = Object.getOwnPropertyDescriptor(globalThis, "Infinity");
console.log(d.writable, d.enumerable, d.configurable);

// A binding created through the object is read by the bare name.
globalThis.createdLater = 41;
console.log(typeof createdLater, createdLater + 1);

// And a function, called by the bare name.
globalThis.hostFn = function (v) { return v * 2; };
console.log(hostFn(21));

// Deleting the property removes the binding: `typeof` is safe again and a
// bare read throws.
delete globalThis.createdLater;
console.log(typeof createdLater);
try {
  createdLater;
  console.log("no throw");
} catch (e) {
  console.log(e.name);
}

// A name that was never created: `typeof` answers "undefined" (13.5.3 step 1)
// and a read is a ReferenceError at the moment of use (6.2.5.5 step 2).
console.log(typeof neverAnywhere);
try {
  neverAnywhere;
  console.log("no throw");
} catch (e) {
  console.log(e.name);
}

// The feature-detection idiom, which is the reason this matters: a browser
// global is absent here, and the guard has to compile and be false.
if (typeof __DEVTOOLS_HOOK__ !== "undefined") {
  console.log("unreachable");
} else {
  console.log("guarded");
}

// `globalThis` is its own property of itself.
console.log(globalThis.globalThis === globalThis);

// A builtin is reachable both ways and is the same object.
console.log(globalThis.JSON === JSON, typeof globalThis.parseInt);

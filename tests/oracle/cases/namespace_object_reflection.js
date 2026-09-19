// The four NAMESPACE OBJECTS — `Math` (21.3.1), `JSON` (25.5), `Atomics`
// (25.4.2) and `Reflect` (28.1) — are ordinary objects whose [[Prototype]] is
// %Object.prototype%. Each is built with a root shape of its own so a site
// reading `Math.sqrt` shares no transition tree with `point.x`, and that shape
// used to carry NO prototype at all: `Object.getPrototypeOf(Math)` had nothing
// to answer with and took the "a plain object whose root shape names no
// prototype" exit, which aborts the process rather than throwing. `Reflect`
// named `null`, which is a different wrong answer — a deliberate chain cut.
//
// What this pins is that the namespaces are ordinary in exactly the ways a
// reflective walk over the globals asks about: they have a prototype, they
// inherit 20.1.3's members from it, their `@@toStringTag` survives the chain,
// and their own keys are still their members and nothing else.
const namespaces = [Math, JSON, Atomics, Reflect];

console.log(namespaces.map((ns) => Object.getPrototypeOf(ns) === Object.prototype).join(","));
console.log(namespaces.map((ns) => Object.getPrototypeOf(ns) === null).join(","));
console.log(namespaces.map((ns) => typeof ns.hasOwnProperty).join(","));
console.log(namespaces.map((ns) => typeof ns.propertyIsEnumerable).join(","));
console.log(namespaces.map((ns) => Object.prototype.toString.call(ns)).join(","));

// 21.3.1's members are own, non-enumerable, and the chain adds none of them.
console.log(Math.hasOwnProperty("PI"), Math.hasOwnProperty("hasOwnProperty"));
console.log(JSON.hasOwnProperty("parse"), JSON.hasOwnProperty("stringify"));
console.log(Atomics.hasOwnProperty("load"), Reflect.hasOwnProperty("ownKeys"));
console.log(Object.keys(Math).length, Object.keys(JSON).length, Object.keys(Reflect).length);
console.log(JSON.stringify(Object.getOwnPropertyNames(JSON)));

// A walk over the chain terminates, which is the property the missing
// prototype destroyed: `getPrototypeOf` aborted before a loop could be
// noticed.
function chain(v) {
  const out = [];
  for (let p = Object.getPrototypeOf(v); p !== null; p = Object.getPrototypeOf(p)) {
    out.push(p === Object.prototype ? "Object.prototype" : "?");
  }
  return out.join(">");
}
console.log(namespaces.map(chain).join(" "));

// And the reflective members a feature detector actually calls answer rather
// than abort, for every one of them.
for (const ns of namespaces) {
  Object.getOwnPropertyNames(ns);
  Object.getOwnPropertyDescriptors(ns);
  Object.getOwnPropertySymbols(ns);
  Reflect.ownKeys(ns);
  Object.isExtensible(ns);
  Object.isFrozen(ns);
  Object.isSealed(ns);
}
console.log("reflected");

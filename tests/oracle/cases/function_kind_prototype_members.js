// %GeneratorFunction.prototype%, %AsyncFunction.prototype% and
// %AsyncGeneratorFunction.prototype% (27.3.3, 27.7.3, 27.4.3) are ORDINARY
// objects whose [[Prototype]] is %Function.prototype%. bronze's plain-object
// walk stops at a function link, so a member inherited through that link —
// `call`, `toString`, `length`, even `hasOwnProperty` from the end of the
// chain — read as a miss, and for the 20.2.3 names the miss was a process
// abort rather than a value.
//
// What this pins is that the three inherit exactly what 20.2.3 and 20.1.3 put
// on their chain, with themselves as the receiver: `toString` is 20.2.3.5's
// step-1 TypeError because the receiver is not callable, `length` and `name`
// are %Function.prototype%'s own 0 and "", and the ordinary members past it
// answer as for any object.

const GF = Object.getPrototypeOf(function* () {});
const AF = Object.getPrototypeOf(async function () {});
const AGF = Object.getPrototypeOf(async function* () {});
const protos = [GF, AF, AGF];

console.log(protos.map((p) => Object.getPrototypeOf(p) === Function.prototype).join(","));
console.log(protos.map((p) => typeof p).join(","));
console.log(protos.map((p) => Object.getOwnPropertyNames(p).sort().join("+")).join(","));

// 20.2.3's members, reached through the function link.
console.log(protos.map((p) => p.call === Function.prototype.call).join(","));
console.log(protos.map((p) => p.apply === Function.prototype.apply).join(","));
console.log(protos.map((p) => p.bind === Function.prototype.bind).join(","));
console.log(protos.map((p) => p.toString === Function.prototype.toString).join(","));

// 20.2.3.5 step 1: not callable, so a TypeError — through the inherited
// member and through the explicit `.call` form alike.
for (const p of protos) {
  try {
    p.toString();
    console.log("no throw");
  } catch (e) {
    console.log("toString:", e instanceof TypeError);
  }
  try {
    p.call(null);
    console.log("no throw");
  } catch (e) {
    console.log("call:", e instanceof TypeError);
  }
  try {
    Function.prototype.toString.call(p);
    console.log("no throw");
  } catch (e) {
    console.log("toString.call:", e instanceof TypeError);
  }
}

// 20.2.3: %Function.prototype% has `length` 0 and `name` "", and the three
// have no own pair of their own to shadow them.
console.log(protos.map((p) => p.length).join(","), JSON.stringify(protos.map((p) => p.name)));
console.log(protos.map((p) => Object.prototype.hasOwnProperty.call(p, "length")).join(","));

// %Object.prototype% past the function link.
console.log(protos.map((p) => typeof p.hasOwnProperty).join(","));
console.log(protos.map((p) => p.hasOwnProperty("constructor")).join(","));
console.log(protos.map((p) => p.hasOwnProperty("call")).join(","));
console.log(protos.map((p) => p.isPrototypeOf(function* () {})).join(","));
// 20.1.3.5 walks the ARGUMENT's chain, and a function link on it is a real
// object a program may hold: `Function.prototype`, a class's parent, one of
// the three above. Each is found from below rather than skipped.
class Base {}
class Derived extends Base {}
console.log(
  Function.prototype.isPrototypeOf(function () {}),
  Function.prototype.isPrototypeOf(async () => {}),
  Base.isPrototypeOf(Derived),
  Derived.isPrototypeOf(Base),
  Object.prototype.isPrototypeOf(Derived),
  AF.isPrototypeOf(async () => {}),
  GF.isPrototypeOf(async () => {}),
);
console.log(protos.map((p) => Object.prototype.toString.call(p)).join(","));

// A name nothing on the chain carries is an ordinary miss.
console.log(protos.map((p) => p.nothingHere).join(","));

// The prototypes' own members still answer first: `constructor` is the kind's
// constructor, and `prototype` is the instance prototype the two generator
// kinds carry and the async kind does not (27.7.3 gives it none).
console.log(protos.map((p) => p.constructor.name).join(","));
console.log(typeof GF.prototype, typeof AGF.prototype, AF.prototype);

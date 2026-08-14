// %GeneratorFunction.prototype%'s @@toStringTag (ECMA-262 27.3.3.3).
//
// A generator function's [[Prototype]] is %GeneratorFunction.prototype%, not
// %Function.prototype% (15.5.4 InstantiateGeneratorFunctionObject step 4), and
// 27.3.3.3 gives that object an @@toStringTag of "GeneratorFunction". So
// 20.1.3.6's step 15 finds a tag on a generator function where it finds none
// on an ordinary one, and the builtin "Function" of step 6 is overridden —
// which is the one observable consequence of the distinct prototype and the
// thing this case pins.
//
// The GENERATOR OBJECT's tag is a different property on a different object
// (27.5.1.5, "Generator"); `cases/to_string_tag_builtins` pins that one.
const ts = Object.prototype.toString;
function* counter() { yield 1; }
console.log(ts.call(counter));

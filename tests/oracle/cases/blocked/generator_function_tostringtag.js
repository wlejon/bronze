// BLOCKED: bronze has no %GeneratorFunction.prototype%.
//
// 27.3.3.3 gives that object an @@toStringTag of "GeneratorFunction", and a
// generator function's [[Prototype]] is that object rather than
// %Function.prototype% (27.3.4 CreateDynamicFunction / 15.5.4
// InstantiateGeneratorFunctionObject step 4). So 20.1.3.6's step 15 finds a
// tag on a generator function where it finds none on an ordinary one, and the
// builtin "Function" of step 6 is overridden.
//
// bronze creates every callable as the same kind of object, and the generator
// distinction lives in the IL function's body rather than in a prototype
// chain — so there is nowhere for this tag to hang. The GENERATOR OBJECT's
// tag is a different property on a different object (27.5.1.5, "Generator")
// and bronze does have that one; `cases/to_string_tag_builtins` pins it.
//
// Answering "Function" here would be the plausible-but-wrong kind of answer,
// so it is left to fail loudly rather than pinned.
//
// Unblocking this means giving a generator function a distinct prototype
// object of its own — at which point %AsyncFunction.prototype% (27.7.3) is
// the same shape of problem and belongs in the same edit.
const ts = Object.prototype.toString;
function* counter() { yield 1; }
console.log(ts.call(counter));

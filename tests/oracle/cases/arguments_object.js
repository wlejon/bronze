// The `arguments` object.
//
// From ECMA-262 10.2.11 (FunctionDeclarationInstantiation), 10.4.4
// (arguments exotic objects) and 15.3 (arrow functions):
//
// 1. `arguments.length` is the number of arguments the caller actually
//    PASSED, not the number of parameters declared:
//    CreateUnmappedArgumentsObject step 3 sets `length` to `len`, the length
//    of the argument list. So `two(1)` sees 1 and `two(1, 2, 3)` sees 3, even
//    though `two` declares two parameters and reads `b` as `undefined`.
// 2. Indices are ordinary own properties, so an index past the end reads
//    `undefined` like any missing property.
// 3. Step 7 of CreateUnmappedArgumentsObject installs %Array.prototype.values%
//    as @@iterator, so `for (const a of arguments)` walks the values.
// 4. An arrow function is NOT given an `arguments` binding: 10.2.11 runs only
//    for ordinary functions, and 15.3's note says an arrow's `arguments`
//    refers to the one in its enclosing scope — exactly the rule it follows
//    for `this`. A nested ORDINARY function does bind its own, and shadows.
// 5. 10.2.11 step 22 builds the object only when `arguments` is not already
//    bound: a parameter of that name wins and no object exists.
// 6. `callee` is an own property of the arguments object (10.2.11 step 6 puts
//    it there, as the accessor pair whose halves are %ThrowTypeError%) and of
//    nothing else — so an ordinary array has none, and reading it off one is
//    the plain `undefined` any absent property gives. That is worth pinning
//    because bronze's `arguments` IS an array, which makes the property the
//    only thing that can tell the two receivers apart; reading `callee` off a
//    real arguments object is a hard error naming the gap
//    (`cases/blocked/arguments_callee`).
//
// The writes below are pinned against CreateUnmappedArgumentsObject, the
// UNMAPPED object: writing `arguments[0]` does not write the parameter. bronze
// builds that object always, where a spec engine builds the mapped one (10.2.11
// step 21) for a non-strict function with a simple parameter list. That is a
// deliberate divergence — and this file pins the unmapped answer the spec
// defines for it.

function count() {
  return arguments.length;
}
console.log(count(), count("a"), count(1, 2, 3, 4, 5));

function at() {
  return arguments[0] + "|" + arguments[1] + "|" + arguments[2];
}
console.log(at("a"));
console.log(at("a", "b"));
console.log(at("a", "b", "c"));

function two(a, b) {
  return arguments.length + ":" + a + "," + b;
}
console.log(two(1), two(1, 2), two(1, 2, 3));

function total() {
  let t = 0;
  for (let i = 0; i < arguments.length; i++) {
    t += arguments[i];
  }
  return t;
}
console.log(total(), total(5), total(1, 2, 3, 4));

function joined() {
  const parts = [];
  for (const a of arguments) {
    parts.push(a);
  }
  return parts.join("-");
}
console.log(joined(1, 2, 3));
console.log("[" + joined() + "]");

function withRest(a, ...rest) {
  return arguments.length + ":" + a + ":" + rest.length;
}
console.log(withRest(1), withRest(1, 2), withRest(1, 2, 3));

function unmapped(a) {
  arguments[0] = "written";
  return a + "/" + arguments[0];
}
console.log(unmapped("original"));

function outer() {
  const inner = () => arguments.length;
  return inner();
}
console.log(outer(7, 8, 9));

function tag(label) {
  const fmt = () => label + ":" + arguments[1];
  return fmt();
}
console.log(tag("x", "y"));

function deep() {
  const a = () => {
    const b = () => arguments[0] + arguments.length;
    return b();
  };
  return a();
}
console.log(deep("d", "e"));

function outerFn() {
  function innerFn() {
    return arguments.length;
  }
  return innerFn(9) + ":" + arguments.length;
}
console.log(outerFn(1, 2, 3));

const asExpression = function () {
  return arguments.length;
};
console.log(asExpression(1, 2));

class Adder {
  sum() {
    let s = 0;
    for (let i = 0; i < arguments.length; i++) {
      s += arguments[i];
    }
    return s;
  }
}
console.log(new Adder().sum(1, 2, 3), new Adder().sum());

function shadowedByParam(arguments) {
  return arguments;
}
console.log(shadowedByParam("the parameter"));

console.log([].callee, [1, 2].callee);

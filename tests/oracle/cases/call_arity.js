// Calls whose argument count is not the callee's parameter count.
//
// There is no arity error in JavaScript, and that is the whole of this case.
//
// From ECMA-262:
//
// 1. 10.2.11 FunctionDeclarationInstantiation binds every parameter the call
//    did not reach to `undefined`. A parameter is not optional because it was
//    written with a default — every parameter is optional, and a default is
//    only a value to use when the binding would otherwise be `undefined`.
// 2. 8.6.2, again: the default fires on `undefined` and on nothing else, so a
//    call that explicitly passes `undefined` takes the default and one that
//    passes `null` does not.
// 3. Extra arguments are evaluated — their side effects happen — and then
//    dropped. `arguments` is where a function that wants them looks; the named
//    parameters do not grow.
// 4. 15.1.5 ExpectedArgumentCount is `length`: the parameters BEFORE the first
//    one with a default or a rest, which is a fact about the text and not
//    about any call.
function two(a, b) { return String(a) + "," + String(b); }
console.log(two(1));
console.log(two(1, 2, 3));
console.log(two());

function withDefault(a, b = 5) { return String(a) + "," + String(b); }
console.log(withDefault(1));
console.log(withDefault(1, undefined));
console.log(withDefault(1, null));
console.log(withDefault(1, 2, 3));

function withRest(a, ...r) { return String(a) + ":" + r.length; }
console.log(withRest());
console.log(withRest(1));
console.log(withRest(1, 2, 3));

// Rule 3: the extra argument's call still happens.
let effects = 0;
function bump() { effects = effects + 1; return effects; }
function one(a) { return a; }
console.log(one(bump(), bump()), effects);

// Rule 4, which is about the declaration and moves with none of the above.
console.log(two.length, withDefault.length, withRest.length);

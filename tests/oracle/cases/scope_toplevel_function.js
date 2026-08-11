// A top-level `function` declaration sees the module's `let` and `const`.
//
// Derived from ECMA-262: a Script's function declarations and its lexical
// declarations are instantiated into the SAME environment record
// (GlobalDeclarationInstantiation, 16.1.7), so the function's [[Environment]]
// is the one holding `count` and `label`. A closure captures the VARIABLE, not
// its value (9.1.1.1), so a write through one name is visible through every
// other, whichever function performs it. Hoisting (step 17 of the same
// algorithm) initialises the function bindings before any statement runs, so a
// call above a declaration resolves.
//
// Pinned here: a read, a write, a compound assign, a call from inside a nested
// function two levels down, and a call above the declaration.

let count = 5;
const label = 'total';

function read() { return count; }
function bump() { count = count + 1; }
function addTo(n) { count += n; }

console.log(read());
bump();
console.log(read());
addTo(4);
console.log(read());
console.log(label);

// Two levels: the inner function reaches past its parent to the module scope,
// and the write it performs is seen by the module-level read below.
function outer() {
  function inner() {
    count = count * 2;
    return count;
  }
  return inner();
}
console.log(outer());
console.log(count);

// Called before its declaration is reached; the binding is already there.
console.log(readLater());
function readLater() { return count - 1; }

// The module scope's binding is one binding: a closure VALUE created at top
// level and a top-level declaration write the same slot.
const alsoBump = function () { count = count + 100; };
alsoBump();
console.log(read());

// A parameter shadows the module binding for the whole body, and the module
// binding is untouched by the shadowed write.
function shadow(count) {
  count = count + 1;
  return count;
}
console.log(shadow(1));
console.log(read());

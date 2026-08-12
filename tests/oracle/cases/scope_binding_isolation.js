// A function's local bindings are its own, and the module top level does not
// inherit them.
//
// Derived from ECMA-262 9.1.1.3: each function call creates a fresh function
// Environment Record whose bindings are unreachable from any other scope, so a
// name declared inside one function says nothing about the same name at module
// level or inside a sibling. Nothing here is exotic JS — the case exists
// because lowering carried the LAST module function's bindings into the top
// level, and the two faces of that are both pinned below:
//
//  - a top-level `let` whose name matches a local of a module function was
//    rejected as `redeclaration of variable '<name>' in same scope`, so this
//    file does not compile at all if the bindings leak; and
//  - a top-level read of such a name resolved to the leaked binding's SSA
//    value id, which in `main` names an unrelated instruction — a plausible
//    number invented out of another function's arithmetic. That half is
//    pinned in tests/lower, where the observable is that the name does not
//    RESOLVE: it lowers to `ref.error` and a
//    warning rather than to a compile error.
//
// The names below are deliberately the ordinary ones — `i`, `acc`, `result`,
// `secret` — because those are what collide in real files.

function g() {
  let secret = 7;
  return secret;
}

function h(p) {
  let acc = p * 2;
  let i = 1;
  return acc + i;
}

function sum(n) {
  let result = 0;
  for (let i = 0; i < n; i++) {
    result = result + i;
  }
  return result;
}

console.log(g());
console.log(h(3));
console.log(sum(5));

// Same names, module scope, different values and even different types. Each
// is a binding of its own.
let secret = 'module';
let acc = 'a';
let i = 'b';
const result = [1, 2];

console.log(secret);
console.log(acc);
console.log(i);
console.log(result);

// The functions still see their own, after the module bindings exist.
console.log(g());
console.log(h(10));

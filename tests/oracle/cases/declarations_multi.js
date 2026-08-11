// Several declarators in one declaration.
//
// Derived from ECMA-262 14.3.1: a LexicalDeclaration is `LetOrConst
// BindingList`, and a BindingList is a comma-separated list of
// LexicalBindings evaluated LEFT TO RIGHT. Each binding is its own binding —
// they share only the keyword — so a later initializer sees an earlier
// binding's value, and a `let` declarator with no Initializer is initialized
// to undefined (14.3.1.2 step 2). `var` (14.3.2) and the `for` header
// (14.7.4) take the same list; the header's semicolons are punctuation of the
// `for` production, not statement terminators (docs/0014 decision 4).

let a = 1, b = 2, c;
console.log(a);
console.log(b);
console.log(c);
c = a + b;
console.log(c);

const p = 'x', q = 'y';
console.log(p + q);

var u = 1, v = u + 1;
console.log(v);

// Left to right: each initializer sees every binding declared before it in
// the same list.
let s1 = 2, s2 = s1 * 3, s3 = s2 + s1;
console.log(s3);

// A single declarator is unchanged, with and without a trailing semicolon.
let single = 9;
console.log(single);

// Mixed types in one list, and a list whose members are captured by a closure
// (each is a binding of its own, so the closure sees the later writes).
let text = 'hi', num = 3, arr = [1];
const show = () => text + num + arr.length;
console.log(show());
num = 4;
console.log(show());

// The `for` header takes the same BindingList. Both bindings are scoped to
// the loop, and the header's own semicolons still terminate its three parts.
let total = 0;
for (let i = 0, j = 4; i < j; i++) {
  total = total + i;
}
console.log(total);

for (let i = 0, n = 3; i < n; i++) {
  total = total + 10;
}
console.log(total);

// A declarator in the header with no initializer, and one whose initializer
// reads the previous declarator.
for (let i = 0, limit = 3, seen; i < limit; i++) {
  seen = i;
  total = total + seen;
}
console.log(total);

// Inside a function body, and inside a block. A block's declaration SHADOWS
// the enclosing one for the block's extent and no further (ECMA-262 14.2.2:
// the block's declarations go in a new declarative Environment Record whose
// outer link is the enclosing scope, and the record is discarded on exit) —
// so the outer binding is still there afterwards, holding what it held. A
// later initializer in the inner list sees the inner binding, because the
// inner one is initialized before it.
function f() {
  let x = 1, y = 2;
  {
    let x = 10, z = x + y;
    console.log(z);
  }
  console.log(x);
  {
    let x = 'inner';
    console.log(x);
  }
  return x + y;
}
console.log(f());

// The same at module scope, where the shadowed binding is the module's.
let outerName = 'module';
{
  let outerName = 'block';
  console.log(outerName);
}
console.log(outerName);

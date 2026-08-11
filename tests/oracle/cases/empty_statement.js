// The empty statement.
//
// Derived from ECMA-262 14.4: `EmptyStatement : ;` evaluates to empty and
// does nothing at all. It is a Statement, so it is legal anywhere one is —
// after a declaration's own terminator, on a line by itself, and as the body
// of a loop, where all the work is in the header. It is not a no-op the
// parser may skip quietly: `while (advance()) ;` reads very differently from
// `while (advance())` followed by the next statement as the body, and the
// semicolon is what separates them.
//
// Its own file because it is a grammar production of its own, not an edge of
// the declaration list beside it.

let i = 0;;
;
console.log(i);

;;;
let sum = 0;
while (i < 5) { i++; sum += i; }
console.log(sum);

// As a loop body. `n++ < 3` is true for n = 0, 1, 2 and false for n = 3, and
// the increment happens on the failing test too, so n ends at 4.
let n = 0;
while (n++ < 3) ;
console.log(n);

// A `for` whose body is empty: the update is the loop.
let k = 0;
for (k = 0; k < 3; k++) ;
console.log(k);

for (let j = 0; j < 4; j++) ;
console.log('for done');

// A do-while with an empty body.
let d = 0;
do ; while (++d < 3);
console.log(d);

// As the body of an `if`, where the else arm is the one that runs.
if (false) ; else console.log('else ran');

// Inside a block and inside a function body.
function f() {
  ;
  let r = 1;;
  ;
  return r;
}
console.log(f());
{
  ;
}
console.log('end');

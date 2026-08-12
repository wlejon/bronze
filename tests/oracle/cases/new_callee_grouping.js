// The `new` callee is a MemberExpression (ECMA-262 13.3.5), which is one
// grammar rule with several grouping consequences. tests/parse pins the
// TREES; this pins the VALUES, because a parser that is consistently wrong
// produces a self-consistent tree and only an evaluated result catches it.
//
// Every grouping below is written so that the wrong reading terminates with a
// different printed line rather than a crash: each candidate callee is a real
// constructor and each result carries a tag naming which one ran.

function Alpha() {
  this.tag = "alpha";
}
function Beta() {
  this.tag = "beta";
}

// `new a.b.c()` constructs `a.b.c`. Reading the callee as `a.b` and then
// `.c` off the result would print "alpha" instead of "beta": ECMA-262 13.3.5
// takes the whole MemberExpression before the Arguments.
const reg = { inner: { Make: Beta }, Make: Alpha };
console.log(new reg.inner.Make().tag);

// `new a.b().c` constructs `a.b` and reads `.c` from the instance, because
// the '(' ends the MemberExpression. The instance carries a `next` property
// so the two readings are distinguishable by value and not only by success.
function Holder() {
  this.tag = "holder";
  this.next = "read-off-the-instance";
}
const box = { Make: Holder };
console.log(new box.Make().next);

// `new (f())()` constructs what the call RETURNS. A parenthesized expression
// is a PrimaryExpression, so the call inside belongs to the callee and the
// trailing '()' is the new's ArgumentList.
function pick() {
  return Beta;
}
console.log(new (pick())().tag);

// `new a[i]()` constructs the computed member. The index is an ordinary
// expression evaluated where it stands.
const ctors = [Alpha, Beta];
let i = 1;
console.log(new ctors[i]().tag);
console.log(new ctors[i - 1]().tag);

// A string key spelled as a computed member, which is how three.js's
// TubeGeometry reaches a curve class by name.
const byName = { QuadraticBezierCurve3: Beta };
console.log(new byName["QuadraticBezierCurve3"]().tag);

// `new new F()()` constructs the result of the inner `new`. The inner one
// takes the FIRST argument list; a parser that let the inner `new` run its
// suffix chain would CALL the inner instance instead of constructing it.
function Outer() {
  this.tag = "outer";
}
function MakeCtor(which) {
  this.which = which;
  return Outer;
}
console.log(new new MakeCtor("inner-ran")().tag);

// `new Foo` without an argument list is `new NewExpression`, which ECMA-262
// 13.3.5.1 evaluates with an EMPTY argument list — so it is exactly
// `new Foo()`, arguments and all.
function Counted(a) {
  this.seen = a;
}
const bare = new Counted;
console.log(bare.seen);
console.log(new Counted(7).seen);

// Evaluation order: 13.3.5.1 evaluates the callee before the arguments, so
// the callee's own side effects run first. `order` records the sequence.
const order = [];
function noteCallee() {
  order.push("callee");
  return Alpha;
}
function noteArg() {
  order.push("arg");
  return 1;
}
new (noteCallee())(noteArg());
console.log(order.join(","));

// The callee is a value, so "is this a constructor" is a run-time question
// and a non-constructor is a TypeError rather than a compile error
// (step 1).
const notCallable = { nope: 5 };
try {
  new notCallable.nope();
} catch (e) {
  console.log(e instanceof TypeError);
}

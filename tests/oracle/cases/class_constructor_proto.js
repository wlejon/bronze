// 15.7.14 ClassDefinitionEvaluation step 6.e: `class B extends A` sets B's
// [[Prototype]] to A — the CONSTRUCTOR's, not just B.prototype's to A.prototype.
// That second link is what makes a static member of the base visible on the
// derived class, and what `Object.getPrototypeOf(B) === A` asks. Answering
// %Function.prototype% for every class made every derived constructor look
// like a base one.
class A {
  static make() { return "A.make"; }
  static get tag() { return "A.tag"; }
}
class B extends A {}
class C extends B {}

console.log(Object.getPrototypeOf(B) === A, Object.getPrototypeOf(C) === B);
console.log(Object.getPrototypeOf(A) === Function.prototype);
console.log(B.make(), C.make(), C.tag);

// The instance chain is the OTHER link, and both exist at once.
console.log(Object.getPrototypeOf(B.prototype) === A.prototype);

// The chain from a doubly-derived constructor, walked to its end.
let depth = 0;
for (let p = C; p !== null; p = Object.getPrototypeOf(p)) depth++;
console.log(depth);

// A class with no `extends` links to %Function.prototype%, so `A.call` works
// and `A.make` is A's own.
console.log(typeof A.call, Object.prototype.hasOwnProperty.call(A, "make"),
            Object.prototype.hasOwnProperty.call(B, "make"));

// Extending a BUILTIN links to the builtin constructor, which is what makes
// `MyArray.from` reachable.
class MyArray extends Array {}
console.log(Object.getPrototypeOf(MyArray) === Array, typeof MyArray.from);

// A static member written after the declaration is seen through the link too:
// the link is a live [[Prototype]], not a copy taken at declaration time.
A.later = 7;
console.log(B.later, C.later);

// Shadowing a static: the derived class's own wins, and the base keeps its own.
class D extends A { static make() { return "D.make"; } }
console.log(D.make(), A.make());

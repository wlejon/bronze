// %GeneratorFunction%, %AsyncFunction% and %AsyncGeneratorFunction%
// (27.3, 27.7, 27.4). None of the three is a global: the only way a program
// names one is through a function of that form, which is exactly what the
// `constructor === GeneratorFunction` idiom does when it decides whether a
// value is a generator function. Answering `Function` for all three gave every
// function in the program the same answer.
function* gen() {}
async function asyncFn() {}
async function* asyncGen() {}
function plain() {}

const GF = Object.getPrototypeOf(gen).constructor;
const AF = Object.getPrototypeOf(asyncFn).constructor;
const AGF = Object.getPrototypeOf(asyncGen).constructor;

console.log(GF.name, AF.name, AGF.name);
console.log(GF === AF, AF === AGF, GF === AGF);
console.log(plain.constructor === Function, gen.constructor === GF);

// 27.3.3.1: the constructor's `prototype` is the object every generator
// function inherits from, and that object's `constructor` is the constructor.
console.log(GF.prototype === Object.getPrototypeOf(gen));
console.log(GF.prototype.constructor === GF);

// 27.3.3.3: %GeneratorFunction.prototype.prototype% IS %GeneratorPrototype% —
// the object holding `next`, `return` and `throw`.
console.log(typeof GF.prototype.prototype.next, typeof GF.prototype.prototype.throw);
console.log(GF.prototype.prototype === AGF.prototype.prototype);

// An async function's result is a promise, so 27.7.3 gives its prototype no
// `prototype` property of its own.
console.log("prototype" in AF.prototype, "prototype" in GF.prototype);

// Every function of a form shares one intrinsic, and the three prototypes
// are ordinary objects whose own [[Prototype]] is %Function.prototype%.
function* gen2() {}
console.log(Object.getPrototypeOf(gen2) === GF.prototype);
console.log(Object.getPrototypeOf(GF.prototype) === Function.prototype);
console.log(typeof GF.prototype);

// @@toStringTag names the kind, which is the other way the idiom is written.
console.log(Object.prototype.toString.call(gen), Object.prototype.toString.call(asyncFn),
            Object.prototype.toString.call(asyncGen), Object.prototype.toString.call(plain));

// CALLING one is CreateDynamicFunction — compiling source text at run time —
// which an AOT compiler has nothing to do it with, so all three refuse the way
// `Function` does rather than pretending.
try {
  GF("yield 1");
} catch (e) {
  console.log(e.name);
}

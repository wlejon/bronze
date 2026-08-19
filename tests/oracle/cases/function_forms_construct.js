// What the SOURCE FORM of a function decides: whether it has [[Construct]]
// (10.2.2), and whether it has a `prototype` property at all (10.2.11 creates
// one only for a non-arrow, non-method, non-async form).
//
// An arrow and a method are the same code with the same parameters as a plain
// function expression; only the syntax separates them, so the answer has to be
// carried from the parser. Answering "constructor" for all of them made
// `new (() => {})` succeed and put a `prototype` object on every closure.
const forms = [];
function normal() {}
const arrow = () => {};
const obj = { method() {}, *genMethod() {}, async asyncMethod() {} };
function* gen() {}
async function asyncFn() {}
async function* asyncGen() {}
class Cls {}

forms.push(["normal", normal]);
forms.push(["arrow", arrow]);
forms.push(["method", obj.method]);
forms.push(["genMethod", obj.genMethod]);
forms.push(["asyncMethod", obj.asyncMethod]);
forms.push(["gen", gen]);
forms.push(["asyncFn", asyncFn]);
forms.push(["asyncGen", asyncGen]);
forms.push(["class", Cls]);

for (const [label, fn] of forms) {
  let constructed = "no";
  try {
    new fn();
    constructed = "yes";
  } catch (e) {
    constructed = e.name;
  }
  console.log(label, typeof fn.prototype, constructed);
}

// The own keys follow from the same rule: an arrow has `length` and `name`
// and nothing else, and a plain function has `prototype` as well.
console.log(Object.getOwnPropertyNames(arrow).join(","));
console.log(Object.getOwnPropertyNames(normal).join(","));
console.log(Object.getOwnPropertyNames(obj.method).join(","));
console.log(Object.getOwnPropertyNames(gen).join(","));

// A bound function is not a syntactic form: it takes its constructibility from
// the target (10.4.1.2 gives the exotic object [[Construct]] only when the
// target has one).
let boundArrow = "no";
try { new (arrow.bind(null))(); } catch (e) { boundArrow = e.name; }
console.log(boundArrow, typeof (arrow.bind(null)).prototype);

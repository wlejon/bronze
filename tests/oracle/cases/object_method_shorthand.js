// Object-literal method shorthand — ECMA-262 13.2.5 PropertyDefinition,
// 15.4 MethodDefinition.
//
// `{ m() {} }` defines exactly what `{ m: function () {} }` defines: an own,
// enumerable, writable data property holding an ordinary function object. So
// what this case pins is mostly that nothing about it is special — the property
// appears in `Object.keys`, `this` is the receiver of the call, and a reserved
// word is a legal method name because a method name is an IdentifierName.
//
// The one thing that IS special is invisible from the language: lowering
// registers every function it creates under a module-level symbol, so a method
// called `next` must not be named `next` there. The `next` pair below is the
// regression test for that — a module function and a method with the same
// name, which must stay two different functions.
const counter = {
  n: 0,
  bump(by) { this.n += by; return this.n; },
  get doubled() { return this.n * 2; },
  "spaced name"() { return "spaced"; },
  ["comp" + "uted"]() { return "computed"; },
  toString() { return "counter"; },
};
console.log(counter.bump(2), counter.bump(3));
console.log(counter.doubled);
console.log(counter["spaced name"](), counter.computed());
console.log(Object.keys(counter).join("|"));

// A method is an ordinary property, so `this` is whatever the call site made
// the receiver — which is what lets these chain.
const shapes = {
  items: [],
  add(x) { this.items.push(x); return this; },
  size() { return this.items.length; },
};
console.log(shapes.add(1).add(2).size());

function next() { return "module next"; }
const namedNext = { next() { return "method next"; } };
console.log(next(), namedNext.next());

const reserved = { delete() { return "d"; }, return() { return "r"; }, if() { return "i"; } };
console.log(reserved.delete(), reserved.return(), reserved.if());

// The shorthand is what makes the iterator protocol writable at all: an
// iterator is written this way and nothing else.
const range = {
  from: 1,
  to: 4,
  [Symbol.iterator]() {
    let i = this.from;
    const last = this.to;
    return {
      next() {
        if (i > last) return { value: undefined, done: true };
        const value = i;
        i = i + 1;
        return { value: value, done: false };
      },
    };
  },
};
console.log([...range].join(","));
for (const v of range) console.log("saw", v);

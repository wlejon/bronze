// ECMA-262 10.2.9 SetFunctionName and 10.2.10 SetFunctionLength: the two own
// properties every function object is created with.
//
// `length` is 15.1.5 ExpectedArgumentCount — the formal parameters BEFORE the
// first one with an initializer or the rest parameter — and not the parameter
// count. `function f(a, b = 1, ...c)` has length 1 and three parameters.
//
// `name` is the name the surrounding syntax gives. A declaration and a class
// take the name they declare (14.1.2, 15.7.14 step 15); a function expression
// that wrote its own takes that (15.2.5, which also binds it inside the body);
// a method takes its property key (15.4.5); an accessor takes the key with a
// prefix, "get x" / "set x", which is 10.2.9's third argument.
//
// An anonymous function is "" — except in one of the positions 8.6.2
// NamedEvaluation lists, where the surrounding syntax supplies a name instead:
// a binding's initializer (14.3.1.2, 14.3.2.1), a simple assignment to an
// identifier (13.15.2 step 1.d), and a property definition (13.2.5.5).
//
// Both properties are non-enumerable, non-writable and configurable, so a
// sloppy assignment to either is discarded rather than stored.

function decl(a, b) {}
console.log(decl.name, decl.length);

const expr = function () {};
console.log(expr.name, expr.length);

const namedExpr = function inner(a) { return a; };
console.log(namedExpr.name, namedExpr.length);

const arrow = (a, b) => a + b;
console.log(arrow.name, arrow.length);

let reassigned;
reassigned = () => {};
console.log(reassigned.name, reassigned.length);

// NOT a NamedEvaluation position: 13.15.2's step is over an IdentifierRef, and
// a member expression is not one.
const holder = {};
holder.slot = function () {};
console.log(holder.slot.name === '');

const obj = {
    fromProperty: function () {},
    fromArrow: () => {},
    shorthand(x, y) {},
    keepsItsOwn: function tag() {},
};
console.log(obj.fromProperty.name, obj.fromArrow.name);
console.log(obj.shorthand.name, obj.shorthand.length);
console.log(obj.keepsItsOwn.name);

class Klass {
    constructor(a) {}
    method(a, b) {}
    static stat() {}
    get value() { return 1; }
    set value(v) {}
}
console.log(Klass.name, Klass.length);
console.log(Klass.prototype.method.name, Klass.prototype.method.length);
console.log(Klass.stat.name, Klass.stat.length);
const value = Object.getOwnPropertyDescriptor(Klass.prototype, 'value');
console.log(value.get.name, value.get.length);
console.log(value.set.name, value.set.length);

// 15.1.5, one clause at a time.
function defaulted(a, b = 1, c) {}
console.log(defaulted.length);
function rested(a, ...r) {}
console.log(rested.length);
function onlyRest(...r) {}
console.log(onlyRest.length);
function none() {}
console.log(none.length);
console.log(((a, b, c) => a).length);
console.log((function () {}).name === '');
console.log(((a) => a).name === '');

// Own properties, and present.
console.log('name' in decl, 'length' in decl);
console.log(Object.hasOwn(decl, 'name'), Object.hasOwn(decl, 'length'));
console.log(Object.hasOwn(decl, 'prototype'), Object.hasOwn(decl, 'nothing'));

// Non-writable: discarded in sloppy code, and the property keeps its value.
decl.name = 'other';
decl.length = 99;
console.log(decl.name, decl.length);

// A `static name() {}` DEFINES a property over the one 15.7.14 gave the
// constructor, so the method wins where an assignment could not.
class Shadowed { static name() { return 'method'; } }
console.log(typeof Shadowed.name);
console.log(Shadowed.name());

// 8.6.2 reaches into binding patterns too. 14.3.3.2 IteratorBindingInitialization
// and 14.3.3.3 KeyedBindingInitialization each run a SingleNameBinding's
// Initializer through NamedEvaluation with the binding's own identifier, and
// 15.1.3 does the same for a formal parameter's default — so the name comes
// from the binding, not from the property the value was read out of.
const { fromKey = () => {} } = {};
console.log(fromKey.name);
const [fromIndex = function () {}] = [];
console.log(fromIndex.name);
function defaultedParam(param = () => {}) { return param; }
console.log(defaultedParam().name);
// A nested pattern is not a BindingIdentifier, so the name comes from the
// binding INSIDE it rather than from anything outside.
const { outer: { nested = () => {} } = {} } = {};
console.log(nested.name);
// IsAnonymousFunctionDefinition is false here, so NamedEvaluation never runs
// and the function keeps the name it wrote.
const { keepsOwn = function written() {} } = {};
console.log(keepsOwn.name);

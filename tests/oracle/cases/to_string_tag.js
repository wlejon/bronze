// ECMA-262 20.1.3.6 steps 15 to 17: the @@toStringTag lookup, which is what
// makes the builtin tag a DEFAULT rather than an answer.
//
// Step 15 is `Get(O, @@toStringTag)` — an ordinary property read, so an
// inherited tag counts exactly as an own one does, and a getter would run.
// Step 16 tests the result for Type String, and step 17 uses it only then: a
// number, `undefined` and a symbol are IGNORED rather than converted, which is
// why `{ [Symbol.toStringTag]: 42 }` reads "[object Object]" and never
// "[object 42]".
//
// 20.4.2.14 makes `Symbol.toStringTag` a well-known symbol whose
// [[Description]] is "Symbol.toStringTag". It is a symbol and not the string
// that spells it, so a same-named string property is a different property and
// does nothing.
//
// 20.1.2.11 reports it among the own symbol keys, because an assignment makes
// an ordinary own property (6.1.7.1 orders every symbol key after every string
// one).
const ts = Object.prototype.toString;

const own = {};
own[Symbol.toStringTag] = 'Widget';
console.log(ts.call(own));

// Inherited: step 15's Get walks the chain.
const base = {};
base[Symbol.toStringTag] = 'Base';
const derived = Object.create(base);
console.log(ts.call(derived));

// A nearer tag shadows an inherited one, as any property does.
derived[Symbol.toStringTag] = 'Derived';
console.log(ts.call(derived));

// Step 17 replaces a builtin tag that is not "Object" just as readily.
function callable() {}
callable[Symbol.toStringTag] = 'Callable';
console.log(ts.call(callable));

// Not a String: step 16 fails and the builtin tag stands.
const numTag = {};
numTag[Symbol.toStringTag] = 42;
console.log(ts.call(numTag));
const undefTag = {};
undefTag[Symbol.toStringTag] = undefined;
console.log(ts.call(undefTag));
const symTag = {};
symTag[Symbol.toStringTag] = Symbol('nope');
console.log(ts.call(symTag));
function stillAFunction() {}
stillAFunction[Symbol.toStringTag] = 42;
console.log(ts.call(stillAFunction));

// The key is the SYMBOL. A string that spells it is another property.
const spelled = { 'Symbol(Symbol.toStringTag)': 'Nope', toStringTag: 'Nope' };
console.log(ts.call(spelled));

// The symbol itself.
console.log(typeof Symbol.toStringTag);
console.log(Symbol.toStringTag.description);
console.log(Symbol.toStringTag.toString());
console.log(Symbol.toStringTag === Symbol.toStringTag);
console.log(Symbol.toStringTag === Symbol.iterator);
console.log(Symbol.toStringTag === Symbol('Symbol.toStringTag'));

// An own symbol-keyed property, reported as one.
const syms = Object.getOwnPropertySymbols(own);
console.log(syms.length);
console.log(syms[0] === Symbol.toStringTag);
console.log(Object.keys(own).length);
console.log(Object.getOwnPropertyNames(own).length);
console.log(Symbol.toStringTag in own);
console.log(Symbol.toStringTag in derived);
console.log(Symbol.toStringTag in {});

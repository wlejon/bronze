// The Symbol PRIMITIVE: ECMA-262 6.1.5 (the Symbol type), 20.4.1 (the
// constructor) and 20.4.3 (the prototype members reachable on one).
//
// Everything here is about the VALUE. What a symbol does as a property key is
// symbol_keys.js, and the registry is symbol_registry.js — three files rather
// than one, because a case that covers everything stops naming what it is for.
//
// The one fact the whole type exists for is on line 21: two calls with the same
// description are two DIFFERENT values. A string key can be many things a
// symbol cannot, and it can never be that.
//
// 6.1.5.1 is the other half: a symbol does not coerce. ToString and ToNumber of
// one are both TypeErrors, which is what makes an accidental stringification
// loud instead of quietly producing a name that collides with nothing. The
// MESSAGE is bronze's to choose and is not pinned here; only that it throws,
// and that the throw is catchable.
const a = Symbol("tag");
const b = Symbol("tag");

console.log(typeof a);
console.log(a === b, a === a);
console.log(a == b, a == "Symbol(tag)", a == null);

// 20.4.3.3 toString is SymbolDescriptiveString; 20.4.3.2 description reads
// [[Description]] straight out. 22.1.1.1 step 2 makes `String(sym)` the one
// conversion that is allowed, and it produces the same text.
console.log(a.toString());
console.log(a.description);
console.log(String(a));
console.log(a);

// An ABSENT description and an EMPTY one print alike — 20.4.3.3.1 uses the
// empty string for an absent one — and `.description` is what tells them apart.
const bare = Symbol();
const empty = Symbol("");
console.log(bare.toString(), empty.toString());
console.log(bare.description, JSON.stringify(empty.description));
console.log(bare === empty);
console.log(Symbol(undefined).description);

// 20.4.3.1: the back-pointer is the same object the bare name resolves to.
console.log(a.constructor === Symbol, typeof Symbol, typeof a.toString);
// 20.4.3.4 thisSymbolValue: for a PRIMITIVE receiver it is the receiver, which
// is the whole of what `valueOf` can mean where there are no wrapper objects.
console.log(a.valueOf() === a, typeof a.valueOf());

// 7.1.2 ToBoolean has no Symbol case that answers false, so every symbol is
// truthy — including one with an empty description.
console.log(a ? "truthy" : "falsy", !empty, Boolean(bare));

// 6.1.5.1: no implicit conversion, in every spelling that would reach one.
try {
  console.log("" + a);
} catch (e) {
  console.log("concat throws:", e instanceof TypeError);
}
try {
  console.log(`v=${a}`);
} catch (e) {
  console.log("template throws:", e instanceof TypeError);
}
try {
  console.log(a + a);
} catch (e) {
  console.log("add throws:", e instanceof TypeError);
}
try {
  console.log(a + 1);
} catch (e) {
  console.log("mixed add throws:", e instanceof TypeError);
}

// A symbol is an ordinary value everywhere that does NOT convert it: it goes
// into an array, through a function, into a closure and out of one.
function identity(x) {
  return x;
}
const held = [a, b, bare];
console.log(held.length, held[0] === a, held.indexOf(b), held.lastIndexOf(bare));
console.log(identity(a) === a);
const captured = (function () {
  const inner = a;
  return function () {
    return inner;
  };
})();
console.log(captured() === a);
console.log(held.filter(function (s) { return s === a; }).length);

// 20.4.3 members bronze has not built stay diagnosed rather than answered as
// undefined, so nothing here reads one. `description` on a value that is not a
// symbol is a different question and is not asked either.
let count = 0;
for (let i = 0; i < 3; i++) {
  const fresh = Symbol("loop");
  if (fresh !== a && fresh.description === "loop") count++;
}
console.log(count);

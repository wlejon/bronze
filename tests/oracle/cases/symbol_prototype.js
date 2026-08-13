// `Symbol.prototype` as a holder (ECMA-262 20.4.3), and the one thing that
// separates it from the other three intrinsic prototypes: it is an ORDINARY
// object. 20.4.3 says so in as many words — "it is not a Symbol instance and
// does not have a [[SymbolData]] internal slot" — where 21.1.3 and 22.1.3 say
// the opposite of theirs. So there is no Symbol exotic object in this suite to
// pin beside it, and `Object.getPrototypeOf(sym)` is the only route to it from
// a value.
//
// `cases/symbol_primitive` is about the VALUE and stays the file for that. This
// one is about the object its members live on, which is a question that could
// not be asked while they were handed out beside the value.
//
// From 20.4.2 (properties of the Symbol constructor), 20.4.3 (the prototype),
// 20.4.3.2 (get description), 20.4.3.3 (toString), 20.4.3.4 (valueOf),
// 20.4.3.6 (@@toStringTag) and 20.1.3.5 (Object.prototype.toLocaleString):
//
// 1. One object, reached from every symbol, with `Object.prototype` above it.
// 2. A member read twice is the SAME function object.
// 3. `description` is an ACCESSOR and not a data property, which is the fact a
//    table beside the value could not express at all: `getOwnPropertyDescriptor`
//    reports a `get` and no `value`, and the getter runs against the PRIMITIVE
//    the program wrote rather than against the intrinsic it was found on.
// 4. The @@toStringTag is a real own property holding the string "Symbol",
//    which is where `Object.prototype.toString.call(sym)` gets its tag from.
// 5. 20.4.3 defines no `toLocaleString`, so a symbol inherits
//    `Object.prototype`'s — and that one is `Invoke(O, "toString")`, which
//    lands back on 20.4.3.3.
// 6. Every own property of `Symbol` is non-enumerable (20.4.2).

const p = Symbol.prototype;
const a = Symbol("tag");

// 20.4.3: an ordinary object under `Object.prototype`, and the one every symbol
// reaches.
console.log(Object.getPrototypeOf(p) === Object.prototype, typeof p);
console.log(Object.getPrototypeOf(a) === p, Object.getPrototypeOf(Symbol()) === p);
// 20.4.3.1's back-pointer is the object the bare name resolves to.
console.log(p.constructor === Symbol, Symbol.prototype === p);

// The identity property, for the same reason it matters on a number: a member
// answered from a table can be right about every call and wrong about this.
console.log(a.toString === p.toString, a.valueOf === p.valueOf);
console.log(a.toString === Symbol("other").toString);

// 20.4.3.2 is `get Symbol.prototype.description` — an accessor, non-enumerable
// and configurable, with no `set` half and no `value` field at all.
const d = Object.getOwnPropertyDescriptor(p, "description");
console.log(typeof d.get, d.set === undefined, d.enumerable, d.configurable, "value" in d);

// The getter's receiver is the primitive. An ABSENT description reads
// `undefined` and an EMPTY one reads "", which is the distinction the accessor
// exists to make.
console.log(a.description, Symbol().description, Symbol("").description === "");

// 20.4.3.6: a real own property whose value is the string "Symbol", inherited
// by every symbol and read by 20.1.3.6 step 15.
console.log(p[Symbol.toStringTag], a[Symbol.toStringTag]);
console.log(Object.prototype.toString.call(a));

// The chain continues to `Object.prototype`, and `toLocaleString` is the proof:
// 20.4.3 defines none, so the one a symbol answers with is 20.1.3.5's, whose
// whole body is `Invoke(O, "toString")`.
console.log(typeof a.hasOwnProperty, typeof a.toLocaleString);
console.log(a.toLocaleString());

// 20.4.3.4 thisSymbolValue: a receiver that is not a symbol.
try {
  p.toString.call(1);
} catch (e) {
  console.log("detached toString throws:", e instanceof TypeError);
}
console.log(a.valueOf() === a, a.toString() === "Symbol(tag)");

// 20.4.2: every own property of the constructor is non-enumerable — and still
// an own property, which is what `in` asks.
console.log(Object.keys(Symbol).length, "iterator" in Symbol, "for" in Symbol);

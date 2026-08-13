// The receivers whose tag ECMA-262 puts on a PROTOTYPE rather than in
// 20.1.3.6's own list.
//
// That list (steps 4-14) gives every one of these "Object". They read as
// themselves only because of step 15: 24.1.3.13 puts "Map" on `Map.prototype`,
// 24.2.3.12 puts "Set" on `Set.prototype`, 23.2.3.35 puts an accessor over
// [[TypedArrayName]] on `%TypedArray%.prototype`, 25.1.6.6 puts "ArrayBuffer"
// on `ArrayBuffer.prototype`, 25.3.4.25 puts "DataView" on `DataView.prototype`,
// and 20.4.3.6 puts "Symbol" on `Symbol.prototype`.
//
// The symbol line is the one that no longer stands in for anything: bronze
// builds `Symbol.prototype`, so its tag is a real own property of a real object
// found by the ordinary walk (`cases/symbol_prototype`). The other five
// prototypes are still unbuilt, and rt_prop.cpp answers for them from the heap
// kind — the same VALUE by a different route, which is what the case is for.
//
// `Math` and `JSON` are different in kind and are here to say so: 21.3.1.9 and
// 25.5.3 make the tag an OWN property of those two objects, so nothing is
// standing in for anything.
//
// The typed-array line is the one worth reading twice. 23.2.3.35 is an accessor
// whose answer is the view's own [[TypedArrayName]], so nine views give nine
// tags — "[object TypedArray]" would be wrong for all of them.
const ts = Object.prototype.toString;

console.log(ts.call(new Map()));
console.log(ts.call(new Set()));
console.log(ts.call(new Uint8Array(1)));
console.log(ts.call(new Int16Array(1)));
console.log(ts.call(new Float64Array(1)));
console.log(ts.call(new ArrayBuffer(4)));
console.log(ts.call(new DataView(new ArrayBuffer(4))));
console.log(ts.call(Math));
console.log(ts.call(JSON));
console.log(ts.call(Symbol('s')));

// The same answers read as the ordinary properties they are.
console.log(new Map()[Symbol.toStringTag]);
console.log(new Set()[Symbol.toStringTag]);
console.log(new Uint8Array(1)[Symbol.toStringTag]);
console.log(Math[Symbol.toStringTag]);
console.log(JSON[Symbol.toStringTag]);

// `in` agrees with the read, kind by kind — which is the rule every member
// table in the runtime follows.
console.log(Symbol.toStringTag in new Map());
console.log(Symbol.toStringTag in new Set());
console.log(Symbol.toStringTag in new Uint8Array(1));
console.log(Symbol.toStringTag in new ArrayBuffer(4));
console.log(Symbol.toStringTag in Math);
// 23.1.3 and 22.2.6 define none, which is exactly why 20.1.3.6 keeps a builtin
// tag for an array and a RegExp.
console.log(Symbol.toStringTag in []);
console.log(Symbol.toStringTag in /a/);
console.log(/a/[Symbol.toStringTag]);
console.log([][Symbol.toStringTag]);

// 27.5.1.5 puts "Generator" on %GeneratorPrototype%, which bronze DOES have as
// a real object — so this one is an ordinary prototype walk and not a stand-in.
//
// The generator FUNCTION is a different receiver with a different tag, and
// bronze has not got it: `cases/blocked/generator_function_tostringtag`.
function* counter() { yield 1; }
console.log(ts.call(counter()));

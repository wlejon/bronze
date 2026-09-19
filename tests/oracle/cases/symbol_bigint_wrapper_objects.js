// 7.1.18 ToObject boxes EVERY primitive but `undefined` and `null`, and
// `Object(x)` (20.1.1.1 step 4) is the spelling a program has for it. bronze
// built the String, Boolean and Number boxes and refused the Symbol and BigInt
// ones by name — a process abort, not a TypeError — so any reflective code
// that boxed a symbol (`Object.assign(sym, ...)`, `Object.prototype.hasOwn
// Property.call(sym, k)`, a `for (const k in sym)`) killed the process.
//
// What this pins is that a Symbol object and a BigInt object are what 20.4.3
// and 21.2.3 describe: an ordinary object whose [[Prototype]] is the intrinsic
// prototype and whose one internal slot the prototype's own methods read back,
// which every conversion runs through 7.1.1 ToPrimitive rather than through a
// shortcut off the slot.

const sym = Symbol("marker");
const so = Object(sym);

console.log(typeof so, so instanceof Symbol, Object.getPrototypeOf(so) === Symbol.prototype);
console.log(so.valueOf() === sym, so.toString(), so.description);
console.log(Object.prototype.toString.call(so), Object.keys(so).length, JSON.stringify(so));
// 7.1.1 step 2: Symbol.prototype[@@toPrimitive] answers the slot, whatever
// the hint, so `==` sees the symbol and `+ ""` sees a symbol-to-string refusal.
console.log(so == sym, so === sym, sym == so);
console.log(typeof Symbol.prototype[Symbol.toPrimitive], Symbol.prototype[Symbol.toPrimitive].length);
try {
  console.log(so + "");
} catch (e) {
  console.log("string conversion:", e instanceof TypeError);
}
// A boxed symbol as a property key is the symbol (7.1.19 ToPropertyKey runs
// ToPrimitive with hint string).
const bag = {};
bag[so] = 1;
console.log(bag[sym], Object.getOwnPropertySymbols(bag).length);
// The box is a fresh object each time, and the same slot each time.
console.log(Object(sym) === Object(sym), Object(sym).valueOf() === Object(sym).valueOf());
// Object(symbolObject) is the object itself (step 3: an Object is returned as is).
console.log(Object(so) === so);

// ToObject reached through the built-ins that call it, with a symbol receiver.
console.log(Object.prototype.hasOwnProperty.call(sym, "description"));
console.log(Object.prototype.propertyIsEnumerable.call(sym, "toString"));
const assigned = Object.assign(sym, { a: 1 });
console.log(typeof assigned, assigned.valueOf() === sym, assigned.a);
let keys = 0;
for (const k in sym) keys++;
console.log(keys);

// The prototype's members refuse a plain object that is not a box.
try {
  Symbol.prototype.valueOf.call(Object.create(Symbol.prototype));
  console.log("no throw");
} catch (e) {
  console.log("forged receiver:", e instanceof TypeError);
}

// BigInt: the same box, and the same prototype methods read it back.
const bo = Object(10n);
console.log(typeof bo, bo instanceof BigInt, Object.getPrototypeOf(bo) === BigInt.prototype);
console.log(bo.valueOf() === 10n, bo.toString(), bo.toString(2), Object.prototype.toString.call(bo));
// 7.1.1: no @@toPrimitive on BigInt.prototype, so OrdinaryToPrimitive calls
// `valueOf`, and arithmetic sees the BigInt.
console.log(bo + 1n, bo * 2n, bo == 10n, bo === 10n, bo > 9n);
console.log(String(bo), `${bo}`, bo + "");
try {
  JSON.stringify(bo);
  console.log("no throw");
} catch (e) {
  console.log("JSON of a BigInt object:", e instanceof TypeError);
}
console.log(JSON.stringify({ n: 1, o: Object(2n) === Object(2n) }));
try {
  BigInt.prototype.valueOf.call(Object.create(BigInt.prototype));
  console.log("no throw");
} catch (e) {
  console.log("forged receiver:", e instanceof TypeError);
}
console.log(Object.prototype.hasOwnProperty.call(5n, "toString"), Object.assign(5n, { b: 2 }).b);

// 20.1.3.7 Object.prototype.valueOf is ToObject(this): a fresh box for a
// primitive receiver, never the primitive back.
const boxed = [sym, 7n, 1, "s", true].map((p) => Object.prototype.valueOf.call(p));
console.log(boxed.map((b) => typeof b).join(","));
console.log(boxed[0].valueOf() === sym, boxed[1].valueOf() === 7n, boxed[0] === sym);

// The three older boxes still answer the same way beside the two new ones.
console.log(typeof Object(1), typeof Object("s"), typeof Object(true), Object(1) + 1, Object("s") + "!");

// ArraySpeciesCreate in map and filter, CreateDataPropertyOrThrow into the
// result, and builtins that keep working when the program replaces the
// globals and prototype members they would otherwise reach for.
"use strict";

function show(label, v) { console.log(label, JSON.stringify(v)); }
function err(label, f) {
    try { f(); console.log(label, "no throw"); }
    catch (e) { console.log(label, e.constructor.name); }
}

class MyArray extends Array {}
const m = MyArray.from([1, 2, 3]);
const mm = m.map(x => x * 2);
console.log("subclass map", mm instanceof MyArray, mm.constructor.name, JSON.stringify(mm));
const mf = m.filter(x => x > 1);
console.log("subclass filter", mf instanceof MyArray, JSON.stringify(mf));

class NoSpecies extends Array { static get [Symbol.species]() { return Array; } }
const ns = NoSpecies.from([1, 2]);
console.log("species Array", ns.map(x => x) instanceof NoSpecies, Array.isArray(ns.map(x => x)));

class NullSpecies extends Array { static get [Symbol.species]() { return null; } }
console.log("species null", NullSpecies.from([1]).map(x => x).constructor === Array);

class UndefSpecies extends Array { static get [Symbol.species]() { return undefined; } }
console.log("species undefined", UndefSpecies.from([1]).filter(() => true).constructor === Array);

const ctorUndefined = [1, 2];
ctorUndefined.constructor = undefined;
console.log("constructor undefined", Array.isArray(ctorUndefined.map(x => x)));

const badCtor = [1];
badCtor.constructor = 5;
err("constructor not an object", () => badCtor.map(x => x));

const badSpecies = [1];
badSpecies.constructor = { [Symbol.species]: 7 };
err("species not a constructor", () => badSpecies.map(x => x));

// A species constructor that records its length argument and returns a plain
// object: map writes indices onto it with CreateDataProperty.
const lengths = [];
function Collector(n) { lengths.push(n); this.made = true; }
const viaPlain = [4, 5, 6];
viaPlain.constructor = { [Symbol.species]: Collector };
const collected = viaPlain.map(x => x + 1);
console.log("species called with", JSON.stringify(lengths), collected.made, collected[0], collected[2], collected.length);
const filtered = viaPlain.filter(x => x !== 5);
console.log("filter species length arg", JSON.stringify(lengths), filtered[0], filtered[1], filtered.length);

// A species result that refuses the definition.
function Frozen() { return Object.freeze([]); }
const toFrozen = [1];
toFrozen.constructor = { [Symbol.species]: Frozen };
err("map into frozen", () => toFrozen.map(x => x));
function Sealed() { return Object.seal({}); }
const toSealed = [1];
toSealed.constructor = { [Symbol.species]: Sealed };
err("filter into sealed", () => toSealed.filter(() => true));

// A setter on the result's prototype chain is not run (define, not set).
let setterRan = false;
function WithSetter() { return Object.create({ set 0(v) { setterRan = true; } }); }
const toSetter = [1];
toSetter.constructor = { [Symbol.species]: WithSetter };
const r = toSetter.map(x => x + 1);
console.log("setter not run", setterRan, Object.getOwnPropertyDescriptor(r, "0").value);


console.log("Array species is Array", Array[Symbol.species] === Array);


// A symbol and the things it points at, held live across thousands of
// collections. Under `oracle-gc-stress` every allocation below moves the entire
// live set, so anything reachable only through a raw pointer is a wrong answer
// rather than a crash — which is the failure mode three separate chunks shipped
// before this suite existed.
//
// A symbol is the unusual value here, and the reason is worth stating: it lives
// in the NON-MOVING ARENA (runtime/symbol.h), because a shape node has to point
// at one and a shape may never point into the collected heap. So the symbol
// itself cannot go stale and neither can the description hanging off it. What
// CAN go stale is everything on the other side of a symbol key — the object,
// its slot storage, the value in the slot, and the fresh heap string
// `.description` and `Symbol.keyFor` hand back on every read. Those are what
// this case walks over, in a loop long enough that the collector runs between
// every pair of statements.
const tag = Symbol("gc");
const registered = Symbol.for("gc-registry");

const holder = {};
holder[tag] = "kept";
holder[registered] = { deep: "value" };

let churn = "";
let mismatches = 0;
let descriptions = 0;
for (let i = 0; i < 400; i++) {
  // A fresh object with a symbol-keyed property, read straight back. The write
  // takes a shape transition and may grow the slot storage, which allocates —
  // so a read that used a pointer from before the write is reading dead space.
  const box = {};
  box[tag] = i;
  box.plain = "s" + i;
  if (box[tag] !== i || box.plain !== "s" + i) mismatches++;

  // The description is a fresh heap string per read. Comparing it after the
  // next allocation is what catches one that was not rooted.
  const d = tag.description;
  churn = "x" + i;
  if (d === "gc" && tag.description === "gc") descriptions++;

  // The long-lived object, re-read every iteration: its slot must survive
  // every collection the loop causes, and the object it holds must too.
  if (holder[tag] !== "kept" || holder[registered].deep !== "value") mismatches++;
  if (Symbol.keyFor(registered) !== "gc-registry") mismatches++;
}

console.log(mismatches, descriptions, churn);
console.log(holder[tag], holder[registered].deep);
console.log(tag.description, Symbol.keyFor(registered), registered === Symbol.for("gc-registry"));
console.log(Object.getOwnPropertySymbols(holder).length, Object.keys(holder).length);

// The same again through a CLOSURE, which is where a missing root shows up as a
// value that was right when it was captured and wrong when it was read.
function makeReader(key, value) {
  const inner = {};
  inner[key] = value;
  return function () {
    return inner[key];
  };
}
const readers = [];
for (let i = 0; i < 200; i++) readers.push(makeReader(tag, "r" + i));
let readBack = 0;
for (let i = 0; i < readers.length; i++) {
  if (readers[i]() === "r" + i) readBack++;
}
console.log(readBack, readers.length);

// A symbol used as a key on an object that has been moved to DICTIONARY mode,
// which stores its keys in a table beside the shape rather than in the chain.
// The table lives in the arena and the values live in the object, so the two
// halves age differently.
const dict = {};
const keys = [];
for (let i = 0; i < 40; i++) {
  const k = Symbol("d" + i);
  keys.push(k);
  dict[k] = i * 3;
}
dict.removeMe = 1;
delete dict.removeMe;
let dictSum = 0;
for (let i = 0; i < keys.length; i++) {
  const filler = { pad: "p" + i };
  if (filler.pad !== "p" + i) mismatches++;
  dictSum = dictSum + dict[keys[i]];
}
console.log(dictSum, Object.getOwnPropertySymbols(dict).length, Object.keys(dict).length);
console.log(mismatches);

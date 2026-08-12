// Nothing internal leaks into an enumeration — the guard on the state
// ECMA-262 keeps in INTERNAL SLOTS.
//
// A Map's and a Set's iterators carry [[IteratedMap]], [[MapNextIndex]] and
// [[MapIterationKind]] (24.1.5), a typed array's carries the array-iterator
// slots of 23.1.5, and `String.prototype.matchAll`'s carries [[IteratingRegExp]]
// and friends (22.2.9.1). None of those is a property, so none of them is
// visible to ANY enumeration — not `Object.keys`, not `for-in`, not spread, not
// `JSON.stringify`, and not `Object.getOwnPropertyNames`, which reports own
// string keys regardless of enumerability (20.1.2.10).
//
// bronze once spelled them as properties under `@@`-prefixed names and bought
// back non-enumerability with a rule about the prefix. That rule could never
// buy the fifth, so `getOwnPropertyNames` reported them; they are real fields
// on the object now, and all five hold. The probes below are written against
// the `@@` spelling on purpose — it is what a regression would reintroduce.
//
// That makes this the tripwire for a change to how state is STORED, ENUMERATED
// or CLASSIFIED: a Map's internals would simply start appearing in an
// enumeration, and every other case in this suite would still pass.
//
// It deliberately does NOT pin the full key list of an iterator. bronze's
// iterators carry `next` as an own property where the language puts it on a
// prototype, and pinning that divergence here would make this case about the
// iterator's shape instead of about the leak.
function leaks(o) {
  let bad = "";
  const own = Object.keys(o);
  for (let i = 0; i < own.length; i++) {
    if (own[i].indexOf("@@") === 0) bad = bad + "keys:" + own[i] + ";";
  }
  for (const k in o) {
    if (k.indexOf("@@") === 0) bad = bad + "forin:" + k + ";";
  }
  const names = Object.getOwnPropertyNames(o);
  for (let i = 0; i < names.length; i++) {
    if (names[i].indexOf("@@") === 0) bad = bad + "names:" + names[i] + ";";
  }
  const text = JSON.stringify(o);
  if (text !== undefined && text.indexOf("@@") >= 0) bad = bad + "json;";
  const spread = { ...o };
  const copied = Object.keys(spread);
  for (let i = 0; i < copied.length; i++) {
    if (copied[i].indexOf("@@") === 0) bad = bad + "spread:" + copied[i] + ";";
  }
  return bad === "" ? "clean" : bad;
}

const m = new Map();
m.set("a", 1);
m.set("b", 2);
const s = new Set([1, 2, 3]);

console.log(leaks(m.entries()), leaks(m.keys()), leaks(m.values()));
console.log(leaks(s.values()), leaks(s.entries()));
console.log(leaks("aXbXc".matchAll(/X/g)));

const ta = new Uint8Array([4, 5]);
console.log(leaks(ta[Symbol.iterator]()));

function* pair() {
  yield 1;
  yield 2;
}
console.log(leaks(pair()));

// An object that defines its own iterator hook keeps it out of enumeration
// too — for a different reason, and one this case exists to keep distinct: the
// hook's key is a SYMBOL (20.4.2.5), and `Object.keys`, `for-in`, spread and
// `JSON.stringify` are all defined over string keys.
const custom = { value: 7 };
custom[Symbol.iterator] = function () {
  let done = false;
  const self = this;
  return {
    next: function () {
      if (done) return { value: undefined, done: true };
      done = true;
      return { value: self.value, done: false };
    },
  };
};
console.log(leaks(custom), Object.keys(custom).join(","), JSON.stringify(custom));
console.log([...custom].join(","));

// The counts, which are where the two mechanisms are told apart. `custom` has
// ONE own symbol key — the hook it was given by assignment (13.15.2 reaching
// CreateDataProperty; 20.1.2.11 reports own symbol keys). A Map's iterator and
// a generator object have NONE: their `[Symbol.iterator]` is INHERITED from
// %MapIteratorPrototype% and %GeneratorPrototype% (24.1.5.2 and 27.5.1.2, both
// reaching 27.1.2.1), and their internal slots are not properties of any kind.
// If a slot ever becomes one — string-keyed or symbol-keyed — one of these
// counts or one of the probes above moves, and this case says so before a
// Map's internals become visible somewhere else.
console.log(typeof Symbol.iterator, Object.getOwnPropertySymbols(custom).length);
console.log(Object.getOwnPropertySymbols(m.entries()).length);
console.log(Object.getOwnPropertySymbols(pair()).length);

// And the collections themselves still work, which is the other half of "the
// slots are hidden": hidden is not the same as gone.
console.log([...m.keys()].join(","), [...m.values()].join(","));
console.log([...s].join(","), m.get("b"), m.size, s.size, s.has(3));
const entries = [];
for (const e of m) entries.push(e[0] + "=" + e[1]);
console.log(entries.join(","));
let walked = 0;
for (const x of s) walked = walked + x;
console.log(walked, [...pair()].join(","));

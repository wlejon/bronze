// Nothing internal leaks into an enumeration — the guard on a rule three
// separate mechanisms lean on, only one of which is about iterators.
//
// bronze spells `Symbol.iterator` as the STRING `"@@iterator"` and buys back
// the one property of a real symbol key the protocol needs with a compensating
// rule: an own key beginning with `@@` is created non-enumerable. Two other
// things then took that rule for their own use — a Map's and a Set's iterators
// keep `@@mapTarget`, `@@mapCursor` and `@@mapKind` as internal slots, and
// `String.prototype.matchAll`'s iterator keeps `@@matchAllRegExp` and friends —
// so the rule is now load-bearing for state that has nothing to do with
// iteration protocols at all.
//
// That makes it the thing a change to how keys are STORED, ENUMERATED or
// CLASSIFIED breaks silently: a Map's internal slots simply start appearing in
// `Object.keys` and `for-in`, and every other case in this suite still passes.
// This case is the tripwire. It asks, of every object in the language that has
// internal state kept this way, whether any `@@` name is visible — and the
// answer ECMA-262 gives is "no", because those are not properties at all.
//
// It deliberately does NOT pin the full key list of an iterator. bronze's
// iterators carry `next` as an own property where the language puts it on a
// prototype, and pinning that divergence here would make this case about the
// iterator's shape instead of about the leak.
//
// `Object.getOwnPropertyNames` is not probed either, and that is a divergence
// rather than an oversight: it reports own string keys REGARDLESS of
// enumerability (20.1.2.10), so it does report `@@mapTarget` today. The `@@`
// rule only ever bought non-enumerability, and that is all it can buy — a real
// symbol key would be absent from `getOwnPropertyNames` as well, which is one
// more thing the migration in the blocked case will fix and this one cannot.
function leaks(o) {
  let bad = "";
  const own = Object.keys(o);
  for (let i = 0; i < own.length; i++) {
    if (own[i].indexOf("@@") === 0) bad = bad + "keys:" + own[i] + ";";
  }
  for (const k in o) {
    if (k.indexOf("@@") === 0) bad = bad + "forin:" + k + ";";
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
// too — the same rule, reached through the spelling a program can write.
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

// The `@@` names are STRINGS, and landing the symbol primitive did not turn
// any of them into symbols. If it ever does, these counts move and this case
// says so before a Map's internals become visible somewhere else.
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

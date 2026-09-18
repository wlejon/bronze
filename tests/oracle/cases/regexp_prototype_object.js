// `RegExp.prototype` and `RegExp` as OBJECTS (ECMA-262 22.2.5, 22.2.6).
//
// `RegExp.prototype` used to be a C table standing in for the prototype: a
// RegExp had no [[Prototype]] a program could read, `RegExp.prototype` was a
// named refusal, its five symbol-keyed algorithms were answered beside the
// value, and `class extends RegExp` was refused by name. Now `RegExp.prototype`
// is an ordinary object holding `exec`, `test`, `toString`, `compile`, the ten
// flag accessors (22.2.6.4.1: `undefined` on the prototype itself, a TypeError
// on a non-RegExp) and the five symbol-keyed members; `RegExp` carries
// `escape` and `[Symbol.species]` as ordinary own properties; and every
// instance is an ordinary object with internal slots whose [[Prototype]] comes
// from NewTarget, with `lastIndex` as its one own property. So the case pins
// what a program can now see and could not before: the identities, the sorted
// own-name lists, the descriptors and name/length of the members, subclassing
// with NewTarget, a wrong receiver being a TypeError rather than a crash, and
// the ordinary object machinery — expandos, `Object.keys`, JSON, `for-in`,
// freeze — treating a RegExp as the object it is while `lastIndex` keeps
// 22.2.4.1's attributes.

const names = (o) => Object.getOwnPropertyNames(o).sort().join(",");
const symbols = (o) => Object.getOwnPropertySymbols(o).map((s) => s.description).join(",");
const desc = (o, k) => {
  const d = Object.getOwnPropertyDescriptor(o, k);
  return d === undefined
    ? "absent"
    : (d.get ? "get:" + d.get.name + "/set:" + typeof d.set : "value:" + typeof d.value) +
        " w:" + d.writable + " e:" + d.enumerable + " c:" + d.configurable;
};
const fails = (f) => {
  try {
    f();
    return "no throw";
  } catch (e) {
    return (e instanceof TypeError) + " " + e.name;
  }
};
const RP = RegExp.prototype;

// --- the constructor and its prototype ----------------------------------
console.log("proto is object:", typeof RP, Object.getPrototypeOf(RP) === Object.prototype);
console.log("ctor:", RP.constructor === RegExp, RegExp.name, RegExp.length);
console.log("ctor proto:", Object.getPrototypeOf(RegExp) === Function.prototype);
console.log("RegExp.prototype desc:", desc(RegExp, "prototype"));
// Annex B's legacy statics (`RegExp.$1`, `lastMatch`, ...) are node's and not
// 22.2.5's, so the listing is the clause's own names only.
const legacy = ["input", "lastMatch", "lastParen", "leftContext", "rightContext"];
console.log("RegExp own names:", Object.getOwnPropertyNames(RegExp)
  .filter((n) => !n.startsWith("$") && !legacy.includes(n)).sort().join(","));
console.log("RegExp species:", desc(RegExp, Symbol.species), RegExp[Symbol.species] === RegExp);
console.log("escape:", RegExp.escape.name, RegExp.escape.length, RegExp.escape("a.b"));
console.log("proto own names:", names(RP));
console.log("proto own symbols:", symbols(RP));

// --- descriptors and name/length of the members -------------------------
for (const k of ["exec", "test", "toString", "compile"]) {
  console.log(k + ":", desc(RP, k), RP[k].name, RP[k].length);
}
for (const k of [
  "dotAll", "flags", "global", "hasIndices", "ignoreCase", "multiline", "source", "sticky",
  "unicode", "unicodeSets",
]) {
  const d = Object.getOwnPropertyDescriptor(RP, k);
  console.log(k + ":", desc(RP, k), d.get.name, d.get.length);
}
for (const s of [Symbol.match, Symbol.matchAll, Symbol.replace, Symbol.search, Symbol.split]) {
  console.log(s.description + ":", desc(RP, s), RP[s].name, RP[s].length);
}

// --- the instance -----------------------------------------------------
const re = /a(b)?/gi;
console.log("instance proto:", Object.getPrototypeOf(re) === RP, re instanceof RegExp);
console.log("instance own names:", names(re), symbols(re));
console.log("lastIndex desc:", desc(re, "lastIndex"));
console.log("hasOwn:", re.hasOwnProperty("lastIndex"), re.hasOwnProperty("source"),
  Object.hasOwn(re, "lastIndex"), "lastIndex" in re, "exec" in re, "source" in re);
console.log("keys:", Object.keys(re).length, JSON.stringify(re));
console.log("accessors:", re.source, re.flags, re.global, re.ignoreCase, re.multiline,
  re.dotAll, re.unicode, re.unicodeSets, re.sticky, re.hasIndices);
console.log("toString:", re.toString(), String(re), "" + re, `${re}`);
console.log("tag:", Object.prototype.toString.call(re));
console.log("delete lastIndex:", delete re.lastIndex, re.hasOwnProperty("lastIndex"));

// --- lastIndex is a real data property ---------------------------------
re.lastIndex = "2";
console.log("assigned as written:", typeof re.lastIndex, re.lastIndex);
console.log("exec from 2:", re.exec("abab")?.index, re.lastIndex);
re.lastIndex = -5;
console.log("negative clamps:", re.exec("ab")?.index, re.lastIndex);
re.lastIndex = 1.9;
console.log("fraction truncates:", re.test("xa"), re.lastIndex);
const defined = /x/g;
Object.defineProperty(defined, "lastIndex", { value: 7 });
console.log("defineProperty value:", defined.lastIndex, desc(defined, "lastIndex"));
console.log("redefine enumerable:", fails(() =>
  Object.defineProperty(defined, "lastIndex", { enumerable: true })));

// --- expandos and the ordinary object machinery ------------------------
const tagged = /t/;
tagged.label = "mine";
console.log("expando:", tagged.label, names(tagged), Object.keys(tagged).join(","),
  JSON.stringify(tagged));
const seen = [];
for (const k in tagged) seen.push(k);
console.log("for-in:", seen.join(","));
console.log("still matches:", tagged.test("tt"), "xtx".replace(tagged, "T"));

// --- freeze --------------------------------------------------------------
const frozen = Object.freeze(/f/g);
console.log("frozen:", Object.isFrozen(frozen), Object.isSealed(frozen), desc(frozen, "lastIndex"));
console.log("frozen exec throws:", fails(() => frozen.exec("f")));
console.log("frozen sloppy assign:", (() => { frozen.lastIndex = 3; return frozen.lastIndex; })());
console.log("frozen strict assign:", fails(() => { "use strict"; frozen.lastIndex = 3; }));
const sealed = Object.seal(/s/g);
console.log("sealed:", Object.isSealed(sealed), Object.isFrozen(sealed), sealed.exec("s").index,
  sealed.lastIndex);

// --- the accessors on wrong receivers (22.2.6.4.1) ---------------------
const getter = (k) => Object.getOwnPropertyDescriptor(RP, k).get;
console.log("on the prototype itself:", getter("global").call(RP), getter("source").call(RP),
  getter("flags").call(RP), RP.source, RP.flags);
console.log("on a plain object:", fails(() => getter("global").call({})),
  fails(() => getter("source").call({})), fails(() => getter("flags").call({})));
console.log("on a primitive:", fails(() => getter("sticky").call(1)));
console.log("flags reads properties:", getter("flags").call({
  hasIndices: 1, global: 0, ignoreCase: "", multiline: "y", dotAll: 1, unicode: 0,
  unicodeSets: 0, sticky: 1,
}));
console.log("exec wrong receiver:", fails(() => RP.exec.call({}, "a")),
  fails(() => RP.test.call("a", "a")));
console.log("toString on plain:", RP.toString.call({ source: "q", flags: "y" }),
  fails(() => RP.toString.call(1)));
console.log("symbol wrong receiver:", fails(() => RP[Symbol.replace].call({}, "a", "b")),
  fails(() => RP[Symbol.split].call("a", "a")), fails(() => RP[Symbol.match].call(null, "a")));

// --- the five reached as function objects ------------------------------
console.log("explicit calls:", /b/[Symbol.replace]("abc", "X"), /b/[Symbol.search]("abc"),
  JSON.stringify(/b/[Symbol.split]("abc")), JSON.stringify(/b/g[Symbol.match]("abcb")),
  [.../b/g[Symbol.matchAll]("abcb")].map((m) => m.index).join(","));
console.log("same function object:", /a/[Symbol.replace] === /b/[Symbol.replace],
  /a/.exec === RP.exec, /a/.test === /b/.test);

// --- `RegExp(...)` as a call and a construct (22.2.4.1) -----------------
const literal = /lit/g;
console.log("RegExp(re) returns it:", RegExp(literal) === literal, RegExp(literal, "i") === literal,
  new RegExp(literal) === literal, new RegExp(literal).flags, new RegExp(literal, "m").flags);
console.log("RegExp(re-like):", RegExp("a", "g").flags, RegExp().source, RegExp(undefined, "y").flags);

// --- subclassing with NewTarget ------------------------------------------
class Tagged extends RegExp {
  constructor(source, flags, tag) {
    super(source, flags);
    this.tag = tag;
  }
  get first() { return this.source[0]; }
}
const t = new Tagged("q+", "g", "T");
console.log("subclass:", t instanceof Tagged, t instanceof RegExp,
  Object.getPrototypeOf(t) === Tagged.prototype, t.tag, t.first, t.source, t.flags);
console.log("subclass own:", names(t), desc(t, "lastIndex"));
console.log("subclass matches:", t.test("aqqq"), t.lastIndex, t.exec("qq")?.[0], t.lastIndex);
console.log("subclass in strings:", "xqqy".replace(t, "_"), "aqbqc".split(t).join("|"),
  "aq".search(t), JSON.stringify("qaq".match(t)), [..."qaq".matchAll(t)].length);
console.log("subclass toString:", String(t), Object.prototype.toString.call(t));
console.log("subclass species:", Tagged[Symbol.species] === Tagged);
class Bare extends RegExp {}
const bare = new Bare("z", "y");
console.log("bare subclass:", bare.sticky, bare.flags, Bare.prototype.constructor === Bare,
  Object.getPrototypeOf(Bare) === RegExp, Object.getPrototypeOf(Bare.prototype) === RP);
console.log("extends via Reflect:", Object.getPrototypeOf(Reflect.construct(RegExp, ["r"], Bare)) === Bare.prototype);

// --- overriding a symbol-keyed member is honoured -----------------------
class Loud extends RegExp {
  [Symbol.replace](str, rep) { return "loud:" + RP[Symbol.replace].call(this, str, rep); }
}
console.log("override [Symbol.replace]:", "abc".replace(new Loud("b"), "X"));
const own = /o/g;
own[Symbol.split] = (s) => ["own", s];
console.log("own [Symbol.split]:", "boo".split(own).join(","));

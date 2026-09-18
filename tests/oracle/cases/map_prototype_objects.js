// `Map.prototype`, `Set.prototype`, `WeakMap.prototype`, `WeakSet.prototype`,
// `WeakRef.prototype` and `FinalizationRegistry.prototype` as OBJECTS.
//
// Each used to be a C table standing in for the prototype: an instance had no
// [[Prototype]] a program could read, `Map.prototype` itself was a named
// refusal, and `class extends Map` kept its prototype on a side box. Now each
// intrinsic is an ordinary object chained to `Object.prototype` (24.1.3,
// 24.2.3, 24.3.3, 24.4.3, 26.1.3, 26.2.3), every instance is an ordinary
// object with internal slots whose [[Prototype]] is on its shape, and the
// methods are found by the walk every other object takes. So the case pins
// what a program can now see and could not before: the prototype's identity
// and own-property list, the accessor `size` really is, function identities
// the spec fixes (`[Symbol.iterator]` IS `entries`; a Map's `has` is NOT a
// Set's), NewTarget deciding an instance's prototype, a wrong receiver being a
// TypeError rather than a crash, and the ordinary object machinery — expandos,
// `Object.keys`, JSON, `setPrototypeOf` — treating a Map as the object it is.
//
// Nothing here depends on a collection: a WeakRef's target is held the whole
// way through, and the registry is never given anything to clean up.

const names = (o) => Object.getOwnPropertyNames(o).sort().join(",");
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

// --- Map ---------------------------------------------------------------------
const m = new Map([[1, "one"]]);
console.log(Object.getPrototypeOf(m) === Map.prototype, m instanceof Map, Map.prototype.constructor === Map);
console.log(names(Map.prototype));
console.log(Object.prototype.toString.call(m), Object.prototype.toString.call(Map.prototype));
console.log(desc(Map.prototype, Symbol.toStringTag), Map.prototype[Symbol.toStringTag]);
console.log(desc(Map.prototype, "size"), desc(Map.prototype, "get"), desc(Map.prototype, "constructor"));
console.log(Map.prototype[Symbol.iterator] === Map.prototype.entries, Map.prototype.keys === Map.prototype.values);
console.log(Map.prototype.get.name, Map.prototype.get.length, Map.prototype.set.length, Map.prototype.forEach.length, Map.prototype.clear.length, Map.prototype[Symbol.iterator].name);
console.log(Map.name, Map.length, names(Map), typeof Map.groupBy, Object.getOwnPropertyDescriptor(Map, "groupBy").enumerable);
console.log(Map.prototype.get.call(m, 1), Map.prototype.has.call(m, 2), Object.getOwnPropertyDescriptor(Map.prototype, "size").get.call(m));
console.log(Object.getPrototypeOf(Map.prototype) === Object.prototype, m.hasOwnProperty("get"), "get" in m, m.toString === Object.prototype.toString);

// --- Set ---------------------------------------------------------------------
const s = new Set([1]);
console.log(Object.getPrototypeOf(s) === Set.prototype, s instanceof Set, s instanceof Map, Set.prototype.constructor === Set);
console.log(names(Set.prototype));
console.log(Object.prototype.toString.call(s), Set.prototype[Symbol.toStringTag]);
console.log(Set.prototype.keys === Set.prototype.values, Set.prototype[Symbol.iterator] === Set.prototype.values, Set.prototype.keys.name);
console.log(desc(Set.prototype, "size"), desc(Set.prototype, "add"));
console.log(Set.prototype.add.length, Set.prototype.has.length, Set.prototype.union.length, Set.prototype.forEach.length);
// 24.1.3.7 and 24.2.3.7 are two function objects, and so are the rest of the
// names the two prototypes share; the two `size` getters likewise.
console.log(Map.prototype.has === Set.prototype.has, Map.prototype.delete === Set.prototype.delete, Map.prototype.clear === Set.prototype.clear);
console.log(Map.prototype.forEach === Set.prototype.forEach, Map.prototype.values === Set.prototype.values, Map.prototype.entries === Set.prototype.entries);
console.log(Object.getOwnPropertyDescriptor(Map.prototype, "size").get === Object.getOwnPropertyDescriptor(Set.prototype, "size").get);

// --- wrong receivers: RequireInternalSlot is a TypeError, never a crash -----
console.log(fails(() => Map.prototype.get.call({}, 1)), fails(() => Map.prototype.get.call(s, 1)));
console.log(fails(() => Map.prototype.has.call(s, 1)), fails(() => Set.prototype.has.call(m, 1)), fails(() => Set.prototype.add.call(m, 1)));
console.log(fails(() => Map.prototype.set.call(new WeakMap(), 1, 2)), fails(() => Map.prototype.forEach.call(Object.create(Map.prototype), () => {})));
console.log(fails(() => Object.getOwnPropertyDescriptor(Map.prototype, "size").get.call(s)), fails(() => Object.getOwnPropertyDescriptor(Set.prototype, "size").get.call({})));
console.log(fails(() => Map.prototype.entries.call(undefined)), fails(() => Set.prototype.values.call(null)), fails(() => Map.prototype.clear.call(1)));
console.log(fails(() => WeakMap.prototype.get.call(m, 1)), fails(() => WeakSet.prototype.has.call(new Set(), 1)), fails(() => WeakRef.prototype.deref.call({})));
console.log(fails(() => FinalizationRegistry.prototype.register.call({}, {}, 1)), fails(() => Map()), fails(() => Set()), fails(() => WeakRef({})));

// --- subclassing: super.get, the prototype from NewTarget ------------------
class Loud extends Map {
  get(k) {
    const v = super.get(k);
    return v === undefined ? "none" : v + "!";
  }
}
const l = new Loud([["a", "x"]]);
console.log(Object.getPrototypeOf(l) === Loud.prototype, l instanceof Map, l instanceof Loud, l.get("a"), l.get("b"), l.size, l.has("a"));
console.log(Object.getPrototypeOf(Loud.prototype) === Map.prototype, Object.getPrototypeOf(Loud) === Map, Loud.groupBy === Map.groupBy);
class Tally extends Set {
  add(v) {
    super.add(v);
    this.count = (this.count || 0) + 1;
    return this;
  }
}
const t = new Tally([1, 1, 2]);
console.log(t.size, t.count, t instanceof Set, [...t].join(","), t.union(new Set([3])) instanceof Set, t.union(new Set([3])).size);
function Other() {}
// 24.1.1.1 steps 6-7: the adder is Get(map, "set") on the NEW object, so a
// NewTarget whose prototype has no `set` is a TypeError once there is an
// entry to add — and a subclass's own `add` runs per element (Tally above).
console.log(Object.getPrototypeOf(Reflect.construct(Map, [], Other)) === Other.prototype, fails(() => Reflect.construct(Map, [[[1, 2]]], Other)));
console.log(Object.getPrototypeOf(Reflect.construct(Set, [], Other)) === Other.prototype, fails(() => Reflect.construct(Set, [[1]], Other)), Reflect.construct(WeakMap, [], Other) instanceof Other);
Other.prototype.set = function (k, v) { this.seen = (this.seen || "") + k + "=" + v + ";"; };
console.log(Reflect.construct(Map, [[[1, 2], [3, 4]]], Other).seen, Map.prototype.get.call(Reflect.construct(Map, [[[1, 2]]], Other), 1));
console.log(Object.getPrototypeOf(Reflect.construct(WeakRef, [{}], Other)) === Other.prototype, Reflect.construct(FinalizationRegistry, [() => {}], Other) instanceof Other);
// The subclass instance still iterates, prints and stringifies as the
// collection it is.
console.log([...l].length, JSON.stringify(l), Object.keys(t).join(","));

// --- an ordinary object: expandos, keys, JSON, freeze, setPrototypeOf -------
const e = new Map([[1, 2]]);
e.label = "tag";
console.log(Object.keys(e).join(","), e.label, JSON.stringify(e), JSON.stringify(new Map([[1, 2]])), JSON.stringify({ m: new Set([1]) }));
Object.defineProperty(e, "hidden", { value: 3, enumerable: false });
console.log(Object.getOwnPropertyNames(e).sort().join(","), e.hidden, e.get(1), e.size);
const fr = Object.freeze(new Map());
console.log(Object.isFrozen(fr), Object.isExtensible(fr), fr.set(1, 1) === fr, fr.size);
let forIn = "";
for (const k in e) forIn += k + ";";
console.log(forIn);
const detached = new Map([[1, 2]]);
Object.setPrototypeOf(detached, null);
console.log(Object.getPrototypeOf(detached), detached.get, Map.prototype.get.call(detached, 1), Map.prototype.has.call(detached, 1));
const adopted = Object.create(Map.prototype);
console.log(adopted instanceof Map, typeof adopted.get, fails(() => adopted.get(1)), fails(() => adopted.size));

// --- iterators: prototypes arranged like an array's ------------------------
const mi = new Map([[1, 2]]).entries();
const si = new Set([1]).values();
const ai = [][Symbol.iterator]();
const IteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(ai));
console.log(Object.getPrototypeOf(mi) === Object.getPrototypeOf(new Map().keys()), Object.getPrototypeOf(mi) === Object.getPrototypeOf(new Map().values()));
console.log(Object.getPrototypeOf(si) === Object.getPrototypeOf(new Set().entries()), Object.getPrototypeOf(mi) === Object.getPrototypeOf(si));
console.log(Object.getPrototypeOf(Object.getPrototypeOf(mi)) === IteratorPrototype, Object.getPrototypeOf(Object.getPrototypeOf(si)) === IteratorPrototype);
console.log(Object.prototype.toString.call(mi), Object.prototype.toString.call(si), mi[Symbol.iterator]() === mi);
console.log(JSON.stringify(mi.next()), JSON.stringify(mi.next()), JSON.stringify(si.next()));
// for-of honours a replaced `[Symbol.iterator]` on the instance, and one on a
// subclass prototype, and the intrinsic where nothing replaced it.
const custom = new Map([[1, "a"]]);
custom[Symbol.iterator] = function* () { yield "custom"; };
console.log([...custom].join(","), [...new Map([[1, "a"]])].join(","), [...Map.prototype.entries.call(custom)].join(","));
class Rev extends Set {
  *[Symbol.iterator]() { yield* [...super.values()].reverse(); }
}
console.log([...new Rev([1, 2, 3])].join(","), [...new Set([1, 2, 3])].join(","));

// --- WeakMap / WeakSet -----------------------------------------------------
const key = {};
const wm = new WeakMap([[key, 1]]);
console.log(Object.getPrototypeOf(wm) === WeakMap.prototype, wm instanceof WeakMap, wm instanceof Map, WeakMap.prototype.constructor === WeakMap);
console.log(names(WeakMap.prototype), Object.prototype.toString.call(wm), desc(WeakMap.prototype, Symbol.toStringTag));
console.log(WeakMap.prototype.get.length, WeakMap.prototype.set.length, WeakMap.prototype.set.name, WeakMap.name, WeakMap.length, names(WeakMap));
console.log(WeakMap.prototype.get.call(wm, key), WeakMap.prototype.has.call(wm, {}), WeakMap.prototype.get === Map.prototype.get, "size" in wm, Symbol.iterator in wm);
const ws = new WeakSet([key]);
console.log(Object.getPrototypeOf(ws) === WeakSet.prototype, ws instanceof WeakSet, names(WeakSet.prototype), Object.prototype.toString.call(ws));
console.log(WeakSet.prototype.add.length, WeakSet.prototype.has.call(ws, key), WeakSet.prototype.has === Set.prototype.has, WeakSet.name, names(WeakSet));
class Registry extends WeakMap {
  remember(k, v) {
    super.set(k, v);
    return this;
  }
}
const r = new Registry().remember(key, "kept");
console.log(r instanceof WeakMap, r instanceof Registry, r.get(key), Object.getPrototypeOf(r) === Registry.prototype);
wm.note = "expando";
console.log(Object.keys(wm).join(","), JSON.stringify(wm), Object.getPrototypeOf(WeakMap.prototype) === Object.prototype);

// --- WeakRef / FinalizationRegistry ----------------------------------------
const target = { alive: true };
const wr = new WeakRef(target);
console.log(Object.getPrototypeOf(wr) === WeakRef.prototype, wr instanceof WeakRef, names(WeakRef.prototype), Object.prototype.toString.call(wr));
console.log(WeakRef.prototype.deref.length, WeakRef.prototype.deref.name, WeakRef.name, WeakRef.length, names(WeakRef), desc(WeakRef.prototype, Symbol.toStringTag));
console.log(WeakRef.prototype.deref.call(wr) === target, wr.deref().alive, WeakRef.prototype.constructor === WeakRef);
const reg = new FinalizationRegistry((held) => {});
console.log(Object.getPrototypeOf(reg) === FinalizationRegistry.prototype, reg instanceof FinalizationRegistry, names(FinalizationRegistry.prototype));
console.log(Object.prototype.toString.call(reg), FinalizationRegistry.prototype.register.length, FinalizationRegistry.prototype.unregister.length, FinalizationRegistry.name, FinalizationRegistry.length);
console.log(FinalizationRegistry.prototype.register.call(reg, target, "held", key), FinalizationRegistry.prototype.unregister.call(reg, key), reg.unregister(key));
class Tracked extends WeakRef {
  constructor(t, tag) {
    super(t);
    this.tag = tag;
  }
}
const tr = new Tracked(target, "t1");
console.log(tr instanceof WeakRef, tr.deref() === target, tr.tag, Object.keys(tr).join(","), Object.getPrototypeOf(tr) === Tracked.prototype);
class Cleaner extends FinalizationRegistry {}
const cl = new Cleaner(() => {});
console.log(cl instanceof FinalizationRegistry, cl instanceof Cleaner, cl.register(target, 1) === undefined, cl.unregister(target));
wr.mark = 1;
console.log(Object.keys(wr).join(","), JSON.stringify(wr), Object.getPrototypeOf(WeakRef.prototype) === Object.prototype, Object.getPrototypeOf(FinalizationRegistry.prototype) === Object.prototype);
console.log(target.alive, key !== null);

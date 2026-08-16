// `class extends Map` and `class extends Set`: the derived instance IS the
// collection, not a plain object that lost its internal slots.
//
// ECMA-262 makes the BASE constructor the allocator: 24.1.1.1 runs
// OrdinaryCreateFromConstructor(NewTarget, "%Map.prototype%", « [[MapData]] »),
// so the object carries [[MapData]] and takes its [[Prototype]] from NewTarget
// rather than from `Map`. That single sentence is everything below — `m.set`
// exists because the slot does, and `m instanceof Tagged` holds because the
// prototype came from the subclass.
//
// The interesting collision is a PRIVATE field on such an instance. 15.7.14
// initializes the derived class's fields on the value `super()` bound as
// `this`, which here is the collection itself — so the private brand and the
// entry list live on the same object, and `#tag in o` is a question about an
// object that has no shape to have installed a private name on.
//
// A method DEFINED on the subclass shadows the builtin of the same name
// (`Loud.prototype.has` below), because the prototype chain is consulted before
// the table that stands in for `Set.prototype`. Getting that order backwards is
// the failure this pins: `l.has(7)` would answer the builtin's `true` instead
// of the subclass's string. It reaches the real membership test through a
// collection it holds rather than through `super.has`, because bronze has no
// `Set.prototype` OBJECT for a super-property to be read from.

class Tagged extends Map {
  #tag;
  constructor(tag, entries) {
    super(entries);
    this.#tag = tag;
  }
  get tag() {
    return this.#tag;
  }
  static hasTag(o) {
    return #tag in o;
  }
  bump(k) {
    this.set(k, (this.get(k) || 0) + 1);
    return this;
  }
}

const m = new Tagged("counts", [
  ["a", 1],
  ["b", 2],
]);
console.log(m.size, m.get("a"), m.get("b"));

m.bump("a").bump("c");
console.log(m.size, m.get("a"), m.get("c"), m.tag);

console.log(m instanceof Tagged, m instanceof Map, Object.getPrototypeOf(m) === Tagged.prototype);
console.log(Tagged.hasTag(m), Tagged.hasTag(new Map()), Tagged.hasTag({}));

// The two stores stay separate: an ordinary named property goes in the
// property box beside the entries, never among them.
m.note = "hi";
console.log(m.note, m.size, m.get("note"), Object.keys(m).join(","));
console.log(Object.prototype.toString.call(m));

// An IMPLICIT constructor has to thread its arguments and NewTarget through to
// the base, or the entries argument is lost and `size` reads 0.
class Implicit extends Map {}
const i = new Implicit([
  ["p", 1],
  ["q", 2],
]);
console.log(i.size, i.get("q"), i instanceof Implicit, i instanceof Map);

// Two links of `extends` still reach the same allocator.
class Deeper extends Tagged {
  constructor() {
    super("deep", [["z", 9]]);
  }
}
const d = new Deeper();
console.log(d.size, d.get("z"), d.tag, d instanceof Deeper, d instanceof Tagged, d instanceof Map);

class Uniq extends Set {
  constructor(xs) {
    super(xs);
    this.label = "u";
  }
  sum() {
    let t = 0;
    for (const v of this) t += v;
    return t;
  }
}
const s = new Uniq([1, 2, 2, 3]);
s.add(4);
console.log(s.size, s.has(2), s.sum(), [...s].join("-"), s.label);
console.log(s instanceof Uniq, s instanceof Set, s instanceof Map);
console.log(Object.prototype.toString.call(s));

class Loud extends Set {
  constructor(xs) {
    super(xs);
    this.inner = new Set(xs);
  }
  has(v) {
    return "maybe:" + this.inner.has(v);
  }
}
const l = new Loud([7]);
console.log(l.has(7), l.has(8), l.size);

// None of the above changes what the intrinsics do.
const plain = new Map([["k", 1]]);
console.log(plain.size, plain.get("k"), plain instanceof Tagged, plain instanceof Map);
const plainSet = new Set([1, 1, 2]);
console.log(plainSet.size, plainSet.has(1), plainSet instanceof Uniq, plainSet instanceof Set);

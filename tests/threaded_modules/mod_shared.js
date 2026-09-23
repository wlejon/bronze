// The SHARED-IMAGE module of the threaded-modules test: compiled once, linked
// once, and entered on two threads at the same time — each running it
// against its own heap (tests/threaded_modules/host_shared.cpp says what that
// proves). Every writable table the module owns is exercised, because each
// one held the other thread's heap objects when it was per image:
//
//   * the module ENVIRONMENT cell — `calls` and `history`, top-level
//     bindings the hammer's closures capture and mutate;
//   * the provided-global CACHE — Math, JSON, Array, String read on every call;
//   * the TEMPLATE cells — a tagged template whose strings object must be
//     the same object on every call of one thread;
//   * the INLINE CACHES — property reads and writes on class instances and
//     method calls, latched on each thread against that thread's shapes and
//     function objects.
//
// Silent, like the other two modules: the host prints after the join.

let calls = 0;
const history = [];

class Vec {
  constructor(x, y) { this.x = x; this.y = y; }
  add(o) { return new Vec(this.x + o.x, this.y + o.y); }
  len2() { return this.x * this.x + this.y * this.y; }
}

class Named extends Vec {
  constructor(name, x, y) { super(x, y); this.name = name; }
  label() { return this.name + "(" + this.x + "," + this.y + ")"; }
}

let firstStrings = null;
function tag(strings, ...values) {
  if (firstStrings === null) firstStrings = strings;
  return (strings === firstStrings ? "same" : "DIFFERENT") + ":" + strings.raw.join("|") + ":" + values.join(",");
}

function bump() {
  calls++;
  return calls;
}

globalThis.hammerShared = function (prefix, i) {
  const n = bump();
  let acc = new Vec(0, 0);
  const parts = [];
  for (let j = 0; j < 30; j++) {
    const v = new Named(prefix + j, j, i);
    acc = acc.add(v);
    parts.push(v.label());
  }
  const squares = parts.map((p, k) => k * k).filter((k) => k % 2 === 0);
  history.push(n);
  const t = tag`call${n}of${prefix}`;
  return prefix + i + " n=" + n + " acc=" + acc.x + "," + acc.y + " len2=" + Math.floor(acc.len2() / 1000) +
         " last=" + parts[parts.length - 1] + " sq=" + squares.length + " json=" + JSON.stringify([i, n]) +
         " arr=" + Array.isArray(parts) + " hist=" + history.length + " " + String(t);
};

globalThis.summaryShared = "calls=" + calls + " proto=" + (Object.getPrototypeOf(Named.prototype) === Vec.prototype);

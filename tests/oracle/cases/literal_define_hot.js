// Literal properties and class fields in HOT code: each form runs long enough
// for its inline caches, transition runs and compiled tiers to settle, and
// then a setter (or a read-only property) appears on a prototype the objects
// inherit from — Object.prototype, a prototype a literal names with
// `__proto__`, a base class's prototype. A definition must not change: no
// setter runs for it and every key stays own. The assignment sites beside
// them, for contrast, start meeting the setter at the same moment.

const hasOwn = Object.prototype.hasOwnProperty;
let bad = 0;
let setterCalls = 0;
const badBy = {};
function check(form, o, k, v) {
  if (!hasOwn.call(o, k) || o[k] !== v) {
    bad++;
    badBy[form] = (badBy[form] || 0) + 1;
  }
}
function id(v) {
  return v;
}

function plain(i) {
  return { x: i, y: i + 1, z: i + 2 };
}
function called(i) {
  return { x: id(i), y: id(i + 1), z: id(i + 2) };
}
function shorthand(x, y, z) {
  return { x, y, z };
}
function computed(k, i) {
  return { [k]: i, y: i };
}
function method(i) {
  return { x() { return i; }, z: i };
}
function spread(src) {
  return { ...src };
}
function withProto(P, i) {
  return { __proto__: P, x: i, z: i };
}
function assignX(o, v) {
  o.x = v;
}

class Base {}
class Fields extends Base {
  x = 1;
  y;
  ["z"] = 3;
}
class Own {
  x = 7;
  z = 8;
}

const P = {};
const N = 3000;
for (let i = 0; i < N; i++) {
  if (i === 1000) {
    // The custom prototype first: a setter for x, a read-only z.
    Object.defineProperty(P, "x", {
      set(v) {
        setterCalls++;
      },
      configurable: true,
    });
    Object.defineProperty(P, "z", { value: "P z", writable: false, configurable: true });
  }
  if (i === 1500) {
    Object.defineProperty(Base.prototype, "x", {
      set(v) {
        setterCalls++;
      },
      configurable: true,
    });
    Object.defineProperty(Base.prototype, "y", { value: "base y", writable: false, configurable: true });
  }
  if (i === 2000) {
    Object.defineProperty(Object.prototype, "x", {
      set(v) {
        setterCalls++;
      },
      get() {
        return "inherited";
      },
      configurable: true,
    });
    Object.defineProperty(Object.prototype, "z", { value: "inherited z", writable: false, configurable: true });
  }

  const a = plain(i);
  check("plain", a, "x", i);
  check("plain", a, "z", i + 2);
  const b = called(i);
  check("called", b, "x", i);
  check("called", b, "z", i + 2);
  const c = shorthand(i, i + 1, i + 2);
  check("shorthand", c, "x", i);
  check("shorthand", c, "z", i + 2);
  const d = computed(i % 2 === 0 ? "x" : "z", i);
  check("computed", d, i % 2 === 0 ? "x" : "z", i);
  const e = method(i);
  if (!hasOwn.call(e, "x") || e.x() !== i) bad++;
  check("method", e, "z", i);
  const s = spread(a);
  check("spread", s, "x", i);
  check("spread", s, "z", i + 2);
  const w = withProto(P, i);
  check("proto", w, "x", i);
  check("proto", w, "z", i);
  const f = new Fields();
  check("fields", f, "x", 1);
  check("fields", f, "y", undefined);
  check("fields", f, "z", 3);
  const g = new Own();
  check("own", g, "x", 7);
  check("own", g, "z", 8);

  // The assignment site: through a fresh object whose chain reaches the
  // setters once they exist.
  assignX({}, i);
  assignX({ __proto__: P }, i);
  assignX(new Base(), i);
}

// Every setter call is an assignment's: 3000 - 1000 through P, 3000 - 1500
// through Base.prototype, 3000 - 2000 through Object.prototype.
console.log("bad", bad, JSON.stringify(badBy));
console.log("setter calls", setterCalls);

// The setters gone again: the definitions keep working on what the caches
// now hold, and the assignments store again.
delete Object.prototype.x;
delete Object.prototype.z;
delete P.x;
delete P.z;
delete Base.prototype.x;
delete Base.prototype.y;
let stored = 0;
for (let i = 0; i < 500; i++) {
  const a = plain(i);
  check("after", a, "x", i);
  const f = new Fields();
  check("after", f, "y", undefined);
  const o = {};
  assignX(o, i);
  if (hasOwn.call(o, "x")) stored++;
}
console.log("after", bad, stored, setterCalls);

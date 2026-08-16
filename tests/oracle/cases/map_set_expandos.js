// Ordinary named properties on a Map, a Set, a WeakMap and a WeakSet — the
// store that is NOT the entries.
//
// 24.1.4 makes a Map instance an ordinary object with internal slots. Its
// entries live in `[[MapData]]`, which only `get`/`set`/`has`/`delete`/`size`
// reach; everything spelled `m.k` is the ordinary property machinery, and the
// two stores never see each other. So `m.foo = 1` and `m.set("foo", 2)` are
// two different writes under one name: `m.foo` is 1, `m.get("foo")` is 2, and
// `m.size` counts only the second.
//
// The consequences are the ordinary ones, which is the point — an expando is
// not a special case anywhere. It is an own enumerable string key, so
// `Object.keys`, `for-in`, spread, `Object.assign` and `JSON.stringify` all
// find it (25.5.2.4 SerializeJSONObject walks EnumerableOwnPropertyNames, and
// a Map has no `toJSON`, so a Map with an expando serialises as an object with
// that one key — the empty `{}` everyone quotes is what a Map with NO own
// properties gives). It is an own property, so it SHADOWS the prototype's
// member of the same name and `hasOwnProperty` tells the two apart.
//
// `size` is the one name a write cannot take. 24.1.3.10 / 24.2.3.9 define it as
// an accessor with a getter and no setter, so OrdinarySet finds [[Set]]
// undefined and refuses: quietly in sloppy code (9.1.1.1.5's return), as a
// TypeError in strict. Without that, an expando could shadow the entry count —
// the one way this feature could make a collection lie about itself.

const m = new Map();
m.set("foo", "entry");
m.foo = "property";
console.log(m.foo, m.get("foo"), m.size, Object.keys(m).join(","));
console.log(JSON.stringify(m));

// Own vs inherited: `get` is on the prototype, `foo` is on the instance.
console.log("foo" in m, m.hasOwnProperty("foo"), "get" in m, m.hasOwnProperty("get"));

const seen = [];
for (const k in m) seen.push(k);
console.log(seen.join(","));

// `delete` reaches the property store and leaves the entries alone.
console.log(delete m.foo, m.foo, m.get("foo"), Object.keys(m).length);

// The collection still works with a property on it, and an own property
// shadows the member it is named after.
m.bar = 1;
console.log(typeof m.get, m.get("foo"), m.size);
m.get = 7;
console.log(m.get, typeof m.set);
console.log(JSON.stringify(Object.assign({}, m)));

const s = new Set([1, 2]);
s.tag = "t";
console.log(s.tag, s.size, s.has(1), Object.keys(s).join(","));
console.log(JSON.stringify({ ...s }));

const wm = new WeakMap();
const key = {};
wm.set(key, 1);
wm.note = "n";
console.log(wm.note, wm.get(key), Object.keys(wm).join(","));

const ws = new WeakSet();
ws.note = "w";
console.log(ws.note, Object.keys(ws).join(","));

// A symbol key goes to the same store and stays out of the string-key walks.
const sym = Symbol("q");
m[sym] = 5;
console.log(m[sym], Object.keys(m).length);

// `size` refuses both ways.
s.size = 99;
console.log(s.size);
try {
  (function () {
    "use strict";
    s.size = 99;
  })();
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError, s.size);
}

// A prototype method whose KEY some other class writes as an own property
// (three.js's `WebGLUniforms.prototype.setValue` beside `SingleUniform`'s
// `this.setValue = setter`). The whole-program prediction gives the call site
// a direct target guarded by the method cache: the key being written
// elsewhere says nothing about these receivers. Every way the guarded call
// can stop being the answer is exercised after the site has run hot: the
// method replaced on the prototype, shadowed on one instance, a receiver
// moved to another prototype, receivers of other classes at the same site,
// an accessor installed in the method's place, and the method deleted.

class Table {
  constructor() { this.map = { a: 1, b: 2 }; this.hits = 0; }
  setValue(name, v) { this.hits++; return this.map[name] === undefined ? -1 : this.map[name] * v; }
}

class Cell {
  constructor(k) { this.k = k; this.setValue = function (name, v) { return 'cell:' + name + ':' + (v + k); }; }
}

class Other {
  setValue(name, v) { return 'other:' + name + ':' + v; }
}

// Table's method again, to put back after each change: holding Table's own
// function in a binding would tell the prediction an own slot may hold it.
class Spare {
  setValue(name, v) { this.hits++; return this.map[name] === undefined ? -1 : this.map[name] * v; }
}

// The receivers reach the sites through a closure's cache, as three.js's
// `program.getUniforms()` hands out its WebGLUniforms: lowering's own guess
// has no class for them and the prediction sees Table instances only. `hot`,
// `hotOne` and `hotMoved` only ever see Table instances the program gives no
// own `setValue`, so each has one predicted callee: Table's.
function Program(make) {
  let cached;
  this.getUniforms = function () {
    if (cached === undefined) cached = make();
    return cached;
  };
}
const via = (o) => new Program(() => o);
function hot(p, n) {
  const t = p.getUniforms();
  let s = 0;
  for (let i = 0; i < n; i++) s += t.setValue('a', i) + t.setValue('b', 1) + t.setValue('zz', 2);
  return s;
}
function hotOne(p, name, v) { const t = p.getUniforms(); return t.setValue(name, v); }
function hotMoved(p, name, v) { return p.getUniforms().setValue(name, v); }

// A site that also meets other classes.
function mixed(t, name, v) { return t.setValue(name, v); }

const t = new Table();
const pt = via(t);
console.log(hot(pt, 200), t.hits, hotOne(pt, 'a', 3), hotMoved(via(new Table()), 'b', 4));
console.log(new Cell(3).setValue('x', 4), mixed(new Cell(5), 'y', 1));

// Receivers of other classes at one site.
console.log(mixed(new Other(), 'a', 1), mixed(new Cell(1), 'a', 1), mixed(t, 'b', 3));

// Every later change goes through a computed key read out of JSON, which the
// prediction does not see, so the hot sites keep their one predicted callee
// and it is the method cache's guard that has to notice.
const K = JSON.parse('"setValue"');
const put = (o, v) => Reflect.defineProperty(o, K, { configurable: true, writable: true, value: v });
const proto = Table.prototype;

// Replaced on the prototype after the sites ran hot.
put(proto, function (name, v) { return 'patched:' + name + ':' + v; });
console.log(hotOne(pt, 'a', 7), hot(via(new Table()), 2));
put(proto, Spare.prototype.setValue);
console.log(hotOne(pt, 'a', 7));

// Shadowed on one instance.
const u = new Table();
put(u, (name, v) => 'own:' + name + ':' + v);
const pu = via(u);
console.log(hotOne(pu, 'a', 2), hotOne(pt, 'a', 2), hot(pu, 1));
delete u[K];
console.log(hotOne(pu, 'a', 2));

// Moved to another prototype.
const w = new Table();
const pw = via(w);
console.log(hotMoved(pw, 'b', 1));
Object.setPrototypeOf(w, Other.prototype);
console.log(hotMoved(pw, 'b', 9), hotMoved(pt, 'b', 9));
Object.setPrototypeOf(w, Table.prototype);
console.log(hotMoved(pw, 'b', 9));

// An accessor in the method's place.
let calls = 0;
Object.defineProperty(proto, K, {
  configurable: true,
  get() { calls++; return (name, v) => 'getter:' + name + ':' + v; },
});
console.log(hotOne(pt, 'a', 1), hotOne(via(new Table()), 'b', 2), calls);
put(proto, Spare.prototype.setValue);
console.log(hotOne(pt, 'a', 1), calls);

// Deleted: the call throws a TypeError.
delete proto[K];
try {
  hotOne(pt, 'a', 1);
  console.log('no throw');
} catch (e) {
  console.log(e instanceof TypeError);
}
put(proto, Spare.prototype.setValue);
console.log(hot(pt, 3), t.hits);

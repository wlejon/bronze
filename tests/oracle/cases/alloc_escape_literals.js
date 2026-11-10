// Fresh objects, arrays and environments that a function may keep to itself
// (read back, compared, tag-tested) and ones it lets out in every way a
// program can see one: stored, passed, returned, thrown, captured, used as
// a prototype, frozen, enumerated, serialized. Each runs hot enough to be
// optimized, and allocates enough to collect while its objects are live.

function sum(a, b) {
  const p = { x: a, y: b };
  return p.x + p.y;
}

function sameAndDifferent(a) {
  const p = { v: a };
  const q = { v: a };
  return (p === p ? 1 : 0) + (p === q ? 10 : 0) + (p !== q ? 100 : 0) + (p == null ? 1000 : 0);
}

function kinds(a) {
  const p = { v: a };
  return typeof p + ":" + ("v" in p) + ":" + ("w" in p) + ":" + (p instanceof Object);
}

const kept = [];
function storedInArray(a) {
  const p = { v: a };
  kept.push(p);
  return p.v;
}

let lastSeen = null;
function seen(o) {
  lastSeen = o;
  return o.v;
}
function passed(a) {
  const p = { v: a };
  return seen(p);
}

function returned(a) {
  return { v: a, w: a * 2 };
}

function thrown(a) {
  const p = { v: a };
  if (a % 3 === 0) throw p;
  return p.v;
}
function catcher(a) {
  try {
    return thrown(a);
  } catch (e) {
    return -e.v;
  }
}

function captured(a) {
  const p = { v: a };
  return () => p.v + 1;
}

function asPrototype(a) {
  const proto = { v: a };
  const child = Object.create(proto);
  return child.v;
}

function frozen(a) {
  const p = Object.freeze({ v: a });
  return Reflect.set(p, "v", 0) ? -1 : p.v + (Object.isFrozen(p) ? 1 : 0);
}

function enumerated(a) {
  const p = { a: a, b: a + 1, c: a + 2 };
  let s = "";
  for (const k in p) s += k + p[k];
  return s;
}

function serialized(a) {
  return JSON.stringify({ v: a, w: [a, a + 1], u: undefined });
}

function getterSees(a) {
  const p = {
    v: a,
    get self() {
      return this;
    },
  };
  const q = p.self;
  return q === p ? q.v : -1;
}

function args() {
  const p = { n: arguments.length, first: arguments[0] };
  return p.n + p.first;
}

function selfStoring(a) {
  const p = { v: a, me: null };
  p.me = p;
  return p.me.me.v;
}

function storesOwnClosure(a) {
  const p = { v: a, f: null };
  p.f = function () {
    return p.v;
  };
  return p.f();
}

function arrays(a) {
  const xs = [a, a + 1, a + 2];
  return xs[0] + xs[1] + xs[2] + xs.length;
}

function nested(a) {
  const outer = { inner: { v: a }, w: a + 1 };
  return outer.inner.v + outer.w;
}

function branchy(a) {
  const p = { v: 0 };
  if (a & 1) p.v = a;
  else p.v = -a;
  return p.v;
}

function loopy(n) {
  const acc = { s: 0, c: 0 };
  for (let i = 0; i < n; i++) {
    acc.s += i;
    acc.c++;
  }
  return acc.s * 1000 + acc.c;
}

function churn(n) {
  let last = null;
  for (let i = 0; i < n; i++) last = { i: i, pad: [i, i] };
  return last.i;
}

function heldAcrossCollection(a) {
  const ref = { deep: { v: a } };
  const holder = { r: ref, k: 7 };
  churn(200);
  return holder.r.deep.v + holder.k;
}

let total = 0;
const out = [];
for (let i = 0; i < 3000; i++) {
  total += sum(i, 2);
  total += sameAndDifferent(i);
  total += storedInArray(i);
  total += passed(i);
  total += returned(i).w;
  total += catcher(i);
  total += captured(i)();
  total += asPrototype(i);
  total += frozen(i);
  total += getterSees(i);
  total += args(i, 1, 2);
  total += selfStoring(i);
  total += storesOwnClosure(i);
  total += arrays(i);
  total += nested(i);
  total += branchy(i);
  total += loopy(i % 50);
  total += heldAcrossCollection(i);
  if (i % 1000 === 0) {
    out.push(kinds(i), enumerated(i), serialized(i));
  }
}
console.log(total);
console.log(kept.length, kept[17].v, lastSeen.v);
for (const s of out) console.log(s);

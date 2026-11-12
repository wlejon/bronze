// A plain object keyed by ids: past a few dozen properties an index key moves
// it to a hashed table, and reads and writes by a small integer go straight to
// the entry. Everything a program can see must stay what node says.
function fill(o, from, to) {
  for (let i = from; i < to; i++) o[i] = i * 2;
  return o;
}

// Growth across the move, read back by number and by string.
const a = fill({}, 0, 100);
let sum = 0;
for (let i = 0; i < 100; i++) sum += a[i] + a[String(i)];
console.log(sum, Object.keys(a).length, a[99], a["99"], a[100]);

// Overwrites, null stores, and the same key spelled three ways.
const ids = Object.create(null);
for (let r = 0; r < 3; r++) {
  for (let uid = 40; uid < 120; uid++) ids[uid] = r * 1000 + uid;
  for (let uid = 40; uid < 120; uid += 3) ids[uid] = null;
}
console.log(ids[40], ids[41], ids["42"], ids[43.0], ids[119], ids[120], ids[39]);
console.log(Object.keys(ids).length, 41 in ids, 39 in ids, "41" in ids);

// -0 is the key "0"; 1.5, -1, NaN, Infinity and "01" are names of their own.
const m = fill({}, 0, 40);
m[-0] = "zero";
m[1.5] = "frac";
m[-1] = "neg";
m[NaN] = "nan";
m[Infinity] = "inf";
m["01"] = "padded";
console.log(m[0], m["0"], m[-0], m[1.5], m["1.5"], m[-1], m[NaN], m[Infinity], m["01"], m[1]);
console.log(Object.keys(m).slice(0, 3).join(), Object.keys(m).slice(-5).join());

// Keys past the small integers, and past 2^32.
const big = fill({}, 0, 40);
big[1023] = "a"; big[1024] = "b"; big[70000] = "c"; big[4294967295] = "d"; big[4294967296] = "e";
big[2 ** 53] = "f";
console.log(big[1023], big[1024], big["1024"], big[70000], big[4294967295], big[4294967296], big[2 ** 53]);
big[1024] = "b2"; big[70000] = "c2";
console.log(big[1024], big[70000], Object.keys(big).slice(-6).join());

// Named keys beside the ids; order is indices ascending, then names.
const mixed = { name: "n", kind: "k" };
fill(mixed, 50, 90);
mixed.tail = "t";
mixed[7] = "seven";
console.log(Object.keys(mixed).slice(0, 2).join(), Object.keys(mixed).slice(-3).join(), mixed.name, mixed[7], mixed.tail);
console.log(JSON.stringify(fill({ z: 1 }, 30, 34)));

// delete and re-add.
const d = fill({}, 0, 50);
delete d[10];
delete d["11"];
console.log(d[10], d[11], 10 in d, Object.keys(d).length);
d[10] = "back";
console.log(d[10], Object.keys(d).length, Object.keys(d)[10]);

// An accessor and a non-writable entry among the ids.
const acc = fill({}, 0, 50);
let seen = [];
Object.defineProperty(acc, 5, { get() { return "got5"; }, set(v) { seen.push(v); }, configurable: true });
Object.defineProperty(acc, 6, { value: "fixed", writable: false, enumerable: true });
acc[5] = "x";
acc[6] = "y";
console.log(acc[5], acc[6], seen.join());
(function () {
  "use strict";
  try { acc[6] = "z"; console.log("no throw"); } catch (e) { console.log(e.constructor.name); }
})();

// Frozen and sealed.
const fr = Object.freeze(fill({}, 0, 50));
fr[3] = "no"; fr[60] = "no";
console.log(fr[3], fr[60], Object.isFrozen(fr));
const se = Object.seal(fill({}, 0, 50));
se[3] = "yes"; se[60] = "no";
console.log(se[3], se[60]);

// The chain: an inherited value, an inherited setter, a miss.
const proto = fill({}, 0, 50);
Object.defineProperty(proto, 77, { set(v) { seen.push("p" + v); }, get() { return "p77"; } });
const child = Object.create(proto);
fill(child, 100, 150);
console.log(child[3], child[77], child[100], child[200]);
child[77] = 1;
child[3] = "own";
console.log(seen.join(), child[3], proto[3], Object.keys(child).length);
proto[4] = "changed";
console.log(child[4]);

// A class instance, an array's named side and a function used as a map.
class Bag { constructor() { this.n = 0; } }
const bag = new Bag();
fill(bag, 0, 60);
console.log(bag.n, bag[59], bag instanceof Bag, Object.keys(bag).length);
function fn() {}
fill(fn, 0, 40);
console.log(fn[39], fn.name, Object.keys(fn).length);

// Spread, assign, entries and for-in over a map.
const src = fill({}, 0, 45);
const copy = { ...src, extra: 1 };
let forIn = 0;
for (const k in copy) forIn += k.length;
console.log(Object.keys(copy).length, copy[44], forIn, Object.entries(src)[44].join(), Object.assign({}, src)[20]);

// Values of every kind, and a getter-defined read through the number key.
const vals = fill({}, 0, 40);
vals[1] = undefined; vals[2] = null; vals[3] = "s"; vals[4] = { o: 1 }; vals[5] = 1.5; vals[6] = true;
console.log(vals[1], vals[2], vals[3], vals[4].o, vals[5], vals[6], 1 in vals);

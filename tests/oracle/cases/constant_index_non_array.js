// `o[0]`, `o[3] = v` with a literal index where `o` is no dense Array: a
// typed array, a string, a plain object, a proxy, a primitive. Singly and in
// the runs a matrix or colour routine writes (`c[0] = r; c[1] = g; ...`).
function show(label, f) {
  try { console.log(label, f()); } catch (e) { console.log(label, e.constructor.name); }
}
function get3(o) { return [o[0], o[1], o[2]].join("|"); }
function set4(o, r, g, b, a) { o[0] = r; o[1] = g; o[2] = b; o[3] = a; return o; }
function one(o) { return o[1]; }
function put(o, v) { o[1] = v; return o[1]; }
function far(o) { return o[40]; }
function putFar(o, v) { o[40] = v; return o[40]; }
function strictPut(o, v) { "use strict"; o[1] = v; return o[1]; }

// Typed arrays of every width, in and out of bounds.
for (const T of [Float32Array, Float64Array, Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array]) {
  const t = set4(new T(4), 1.5, -2, 300, 0.25);
  console.log(T.name, get3(t), t[3], one(t), put(t, 7.9), far(t), putFar(t, 5), t.length);
}
const big = new BigInt64Array(4);
show("bigint view", () => { big[0] = 5n; big[1] = -6n; return get3(big); });
show("bigint view number", () => put(big, 1));
const short = new Float32Array(2);
set4(short, 1, 2, 3, 4);
console.log(get3(short), short.length, 3 in short);
const sub = new Uint8Array(new ArrayBuffer(8), 2, 3);
set4(sub, 9, 8, 7, 6);
console.log(get3(sub), new Uint8Array(sub.buffer).join());

// The same sites, one receiver kind after another.
const kinds = [[10, 20, 30, 40], new Float32Array([1, 2, 3, 4]), "abcd", { 0: "p0", 1: "p1", 2: "p2" }, new Uint8Array(2), [1, , 3]];
console.log(kinds.map(get3).join(" ; "));
console.log(kinds.map(one).join(" ; "), kinds.map(far).join(" ; "));

// Strings and other primitives.
show("string write", () => put("abc", "z"));
show("string write strict", () => strictPut("abc", "z"));
show("number", () => [one(5), put(5, 1)].join());
show("boolean strict", () => strictPut(true, 1));
show("undefined read", () => one(undefined));
show("null read", () => get3(null));
show("undefined write", () => put(undefined, 1));
show("null run write", () => set4(null, 1, 2, 3, 4));

// Plain objects: own, inherited, accessor, absent, frozen.
const plain = set4({}, "a", "b", "c", "d");
console.log(get3(plain), Object.keys(plain).join(), plain["1"]);
const proto = { 1: "inherited", get 2() { return "getter " + (this.tag || "?"); }, set 2(v) { this.seen = v; } };
const child = Object.create(proto);
child.tag = "child";
console.log(get3(child), one(child));
set4(child, "x", "y", "z", "w");
console.log(get3(child), child.seen, Object.keys(child).join(), proto[1]);
const frozen = Object.freeze({ 1: "kept" });
show("frozen sloppy", () => put(frozen, "no"));
show("frozen strict", () => strictPut(frozen, "no"));

// Arrays that are not dense where the site looks: holes over an index on the
// chain, short arrays that grow, a frozen array, an array subclass.
const holey = [1, , 3];
const arrayLike = Object.create(Array.prototype);
arrayLike[1] = "from proto";
Object.setPrototypeOf(holey, arrayLike);
console.log(get3(holey), one([]), one([5]), Array.isArray(holey));
delete arrayLike[1];
console.log(get3(holey), put(holey, "own"), get3(holey));
const grow = [];
set4(grow, 1, 2, 3, 4);
console.log(grow.join(), grow.length, putFar([], "x"), putFar([1], "y"));
const fa = Object.freeze([1, 2, 3]);
show("frozen array", () => put(fa, 9));
show("frozen array strict", () => strictPut(fa, 9));
class Vec extends Array { }
const v = set4(new Vec(), 4, 3, 2, 1);
console.log(get3(v), v.length, v instanceof Vec);

// arguments, functions, Map, proxies.
function args() { return get3(arguments) + "/" + put(arguments, "set") + "/" + arguments.length; }
console.log(args("a0", "a1"), args("b0", "b1", "b2"));
function fnRecv() { }
set4(fnRecv, 1, 2, 3, 4);
console.log(get3(fnRecv), fnRecv.length);
const map = new Map();
put(map, "prop");
console.log(one(map), map.size);
const log = [];
const prox = new Proxy({}, {
  get(t, k) { log.push("get " + String(k)); return "via proxy"; },
  set(t, k, val) { log.push("set " + String(k) + "=" + val); return true; },
});
console.log(get3(prox), put(prox, 5));
set4(prox, 1, 2, 3, 4);
console.log(log.join(","));

// Compound forms and deletes on a literal index.
const ta = new Int32Array([5, 6, 7, 8]);
ta[0] += 10; ta[1]++; --ta[2]; ta[3] *= 2;
console.log(ta.join(), delete ta[9], (() => { "use strict"; try { return delete ta[0]; } catch (e) { return e.constructor.name; } })());
const s = "héllo";
console.log(s[1], s[0] + s[4], s[5], "x"[0][0]);

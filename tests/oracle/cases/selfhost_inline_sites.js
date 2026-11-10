// A program's own copies of the self-hosted Array builtins (selfhost_merge.cpp):
// a site such as `arr.map(f)` calls the copy behind `is.plain_array`, and the
// inliner copies the builtin's body and the callback's in. Each section is a
// way the copy could be entered where the registered builtin would not be, or
// with a callback the copy's inlined body does not stand for.

function show(label, v) { console.log(label, JSON.stringify(v)); }
function err(label, f) {
    try { f(); console.log(label, "no throw"); }
    catch (e) { console.log(label, e.constructor.name + ": " + e.message); }
}

// One site, many callbacks: literals, a named function, a callback that is
// replaced between iterations of the loop around the site, and non-callables.
const base = [1, 2, 3, 4];
const callbacks = [x => x + 1, function (x) { return x * 3; }, Math.sqrt, String, Boolean];
for (const cb of callbacks) show("one site", base.map(cb));
let swap = x => x;
for (let r = 0; r < 4; r++) {
    show("swapped " + r, base.filter(swap));
    swap = r % 2 === 0 ? (x => x % 2 === 0) : (x => x > 2);
}
for (const bad of [undefined, null, 1, "f", {}, Symbol.iterator]) {
    err("non-callable " + String(bad), () => base.forEach(bad));
}
// The receiver is read before the callback is checked: an empty array still throws.
err("empty non-callable", () => [].some(42));
// A class constructor is a function to typeof but throws when called.
class K { constructor(x) { this.x = x; } }
console.log("class on empty", JSON.stringify([].map(K)));
err("class on one", () => [1].map(K));

// Every kind of callable.
const bound = function (x) { return this.k * x; }.bind({ k: 10 });
show("bound", base.map(bound));
function* gen() { yield 1; }
show("generator fn", base.map(gen).map(g => typeof g.next));
async function asy(x) { return x; }
show("async fn", base.map(asy).map(p => p instanceof Promise));
const proxied = new Proxy(x => x * 7, { apply(t, self, args) { return "p" + t(...args); } });
show("proxy", base.map(proxied));
const obj = { m(x) { return x - 1; }, get g() { return x => -x; } };
show("method", base.map(obj.m));
show("getter", base.map(obj.g));

// thisArg. (A sloppy callback's `this` is left out: bronze does not box a
// sloppy function's primitive `this`, at any call, which is not this file's
// question.)
function strictThis() { "use strict"; return this === undefined ? "undef" : typeof this; }
show("strict no thisArg", [1].map(strictThis));
show("strict thisArg 5", [1].map(strictThis, 5));
show("arrow ignores thisArg", [1].map(() => typeof this, { a: 1 }));
show("thisArg object", [1, 2].map(function (x) { return this.base + x; }, { base: 100 }));

// Arguments the site passes: fewer than declared, more, the index and array.
show("extra args", base.map((x, i, a, extra) => [x, i, a.length, extra]));
show("index", base.findIndex((x, i) => i === 2));
show("reduce no initial", base.reduce((a, x) => a + x));
show("reduce initial", base.reduce((a, x) => a + x, 100));
show("reduce initial undefined", base.reduce((a, x) => String(a) + x, undefined));
show("reduce extra args", base.reduce((a, x) => a + x, 0, "ignored"));
err("reduce empty", () => [].reduce((a, x) => a + x));
show("reduce empty initial", [].reduce((a, x) => a + x, "init"));

// Receivers the copy must not take: subclasses, own properties, a changed
// prototype, frozen and sealed arrays, array-likes, primitives.
class Sub extends Array {}
const sub = Sub.from([1, 2, 3]);
console.log("subclass map", sub.map(x => x * 2) instanceof Sub, sub.filter(x => x > 1).constructor.name);
const own = [1, 2, 3];
own.map = function () { return "own map"; };
console.log("own method", own.map(x => x));
const ownCtor = [1, 2, 3];
ownCtor.constructor = { [Symbol.species]: function (n) { this.made = n; } };
console.log("own constructor species", JSON.stringify(ownCtor.map(x => x)));
const reproto = [1, 2, 3];
Object.setPrototypeOf(reproto, { map() { return "proto map"; }, __proto__: Array.prototype });
console.log("prototype changed", reproto.map(x => x));
const frozen = Object.freeze([1, 2, 3]);
show("frozen map", frozen.map(x => x + 1));
show("sealed filter", Object.seal([1, 2, 3]).filter(x => x !== 2));
const arrayLike = { length: 3, 0: "a", 1: "b", 2: "c" };
show("call on array-like", Array.prototype.map.call(arrayLike, s => s.toUpperCase()));
show("call on string", Array.prototype.filter.call("abc", c => c !== "b"));
err("call on null", () => Array.prototype.forEach.call(null, x => x));

// The builtin reached other ways: .call, .apply, Reflect.apply, a variable.
const m = Array.prototype.map;
show("variable-held", m.call(base, x => x + 0.5));
show("apply", Array.prototype.some.apply(base, [x => x > 3]));
show("Reflect.apply", Reflect.apply(Array.prototype.every, base, [x => x > 0]));
show("bound builtin", Array.prototype.find.bind(base)(x => x > 2));
const viaName = "findIndex";
show("computed key", base[viaName](x => x === 4));

// Mutation during the walk.
const grow = [1, 2, 3];
show("push during map", grow.map((x, i, a) => { a.push(0); return x; }));
const shrink = [1, 2, 3, 4, 5];
show("pop during filter", shrink.filter((x, i, a) => { a.pop(); return true; }));
const holes = [1, 2, 3, 4];
show("delete ahead", holes.map((x, i, a) => { delete a[i + 1]; return x; }));
const lengthCut = [1, 2, 3, 4, 5];
show("length cut", lengthCut.some((x, i, a) => { a.length = 2; return x > 3; }));
// The array becomes a non-plain one (an own property) mid-walk: the walk goes
// on with the same algorithm.
const becomes = [1, 2, 3];
show("own prop mid-walk", becomes.map((x, i, a) => { if (i === 0) a.extra = 1; return x * 2; }));
// The variable the site reads is reassigned inside the callback.
let target = [1, 2, 3];
show("reassigned receiver", target.map(x => { target = ["z"]; return x; }));
show("after reassign", target.map(x => x + "!"));

// Recursion: a callback that reaches the same site.
function depth(v) { return Array.isArray(v) ? 1 + Math.max(0, ...v.map(depth)) : 0; }
console.log("recursive", depth([1, [2, [3, [4]]], []]));
function flat(v) { return Array.isArray(v) ? v.reduce((a, x) => a.concat(flat(x)), []) : [v]; }
show("recursive reduce", flat([1, [2, [3, [4, 5]]], 6]));

// Callbacks that throw stop the walk at that element.
let visited = 0;
err("throw mid-walk", () => base.forEach(x => { visited++; if (x === 2) throw new RangeError("at 2"); }));
console.log("visited before throw", visited);
err("throw in find", () => base.find(x => { if (x === 3) throw new Error("found 3"); return false; }));

// All eight in one loop, many times, so a site warms whatever the build warms.
let total = 0;
for (let r = 0; r < 2000; r++) {
    const a = [r, r + 1, r + 2, r + 3];
    a.forEach(x => { total += x; });
    total += a.map(x => x * 2)[1];
    total += a.filter(x => x & 1).length;
    total += a.reduce((s, x) => s + x, 0);
    total += a.find(x => x > r + 1);
    total += a.findIndex(x => x === r + 3);
    total += a.some(x => x === r) ? 1 : 0;
    total += a.every(x => x >= r) ? 1 : 0;
}
console.log("all eight", total);

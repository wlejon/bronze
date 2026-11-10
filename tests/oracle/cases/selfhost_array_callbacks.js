// Array.prototype iteration builtins (self-hosted, src/selfhost/builtins.js):
// holes, sparse arrays, callbacks that mutate the array they walk, callbacks
// that throw, and every kind of callable as the callback.
"use strict";

function show(label, v) { console.log(label, JSON.stringify(v)); }
function err(label, f) {
    try { f(); console.log(label, "no throw"); }
    catch (e) { console.log(label, e.constructor.name + ": " + e.message); }
}

// Holes are skipped by forEach/map/filter/some/every/reduce/indexOf and
// visited (as undefined) by find/findIndex.
const holes = [1, , 3, , 5];
const seen = [];
holes.forEach((x, i) => seen.push(i));
show("forEach holes", seen);
const mapped = holes.map(x => x * 10);
show("map holes length", mapped.length);
console.log("map keeps holes", 1 in mapped, 3 in mapped, mapped[4]);
show("filter holes", holes.filter(() => true));
let someCalls = 0;
holes.some(() => { someCalls++; return false; });
console.log("some calls", someCalls);
let everyCalls = 0;
holes.every(() => { everyCalls++; return true; });
console.log("every calls", everyCalls);
const findSeen = [];
holes.find((x, i) => { findSeen.push(i + ":" + x); return false; });
show("find visits holes", findSeen);
console.log("findIndex undefined", holes.findIndex(x => x === undefined));
console.log("indexOf undefined", holes.indexOf(undefined));
console.log("reduce holes", holes.reduce((a, x) => a + x));

// Sparse: a large length with two elements.
const sparse = [];
sparse[3] = "a";
sparse[100] = "b";
const sparseSeen = [];
sparse.forEach((x, i) => sparseSeen.push(i));
show("sparse forEach", sparseSeen);
console.log("sparse map length", sparse.map(x => x).length);
console.log("sparse indexOf", sparse.indexOf("b"));

// The length is read once: elements pushed during the walk are not visited,
// elements deleted ahead of it are skipped, elements changed ahead are seen.
const grow = [1, 2, 3];
const growSeen = [];
grow.forEach((x, i, arr) => { growSeen.push(x); arr.push(x * 100); });
show("push during forEach", growSeen);
console.log("grown to", grow.length);
const shrink = [1, 2, 3, 4, 5];
const shrinkSeen = [];
shrink.forEach((x, i, arr) => { shrinkSeen.push(x); if (i === 0) arr.pop(); });
show("pop during forEach", shrinkSeen);
const del = [1, 2, 3, 4];
show("delete ahead in map", del.map((x, i, arr) => { if (i === 0) delete arr[2]; return x; }));
const chg = [1, 2, 3];
show("change ahead in filter", chg.filter((x, i, arr) => { if (i === 0) arr[1] = 20; return true; }));
const findMut = [1, 2, 3];
console.log("find sees change", findMut.find((x, i, arr) => { if (i === 0) arr[2] = 99; return x === 99; }));
const reduceMut = [1, 2, 3, 4];
console.log("reduce truncation", reduceMut.reduce((a, x, i, arr) => { if (i === 1) arr.length = 3; return a + x; }, 0));

// A callback that throws stops the walk; the exception is the callback's.
let thrownAt = -1;
err("forEach throw", () => [1, 2, 3].forEach((x, i) => { thrownAt = i; if (x === 2) throw new RangeError("stop at " + i); }));
console.log("stopped at", thrownAt);
err("map throw", () => [1, 2].map(() => { throw new SyntaxError("in map"); }));
err("reduce throw", () => [1, 2, 3].reduce(() => { throw "a string"; }));
try { [1].every(() => { throw 42; }); } catch (e) { console.log("every threw", e); }

// Arguments and receivers.
const args = [];
["a", "b"].forEach(function (x, i, arr) { args.push([x, i, arr.length, this === undefined]); });
show("args strict no thisArg", args);
const ctx = { tag: "ctx" };
console.log("thisArg", ["a"].map(function () { return this.tag; }, ctx)[0]);
console.log("thisArg primitive stays", [1].map(function () { return typeof this; }, 5)[0]);
console.log("arrow ignores thisArg", [1].map(() => typeof this, ctx)[0]);
console.log("reduce this undefined", [1, 2].reduce(function (a) { return this === undefined && a; }));

// Every kind of callable.
function* gen() { yield 1; }
async function asy() { return 1; }
const bound = function (x) { return this.k + x; }.bind({ k: 100 });
const obj = { m(x) { return x + 1; }, get g() { return 1; } };
show("generator callback (truthy iterator)", [1, 2].filter(gen).length);
show("async callback (truthy promise)", [1, 2].every(asy));
show("bound callback", [1, 2].map(bound));
show("method callback", [1, 2].map(obj.m));
show("builtin callback", ["1", "2", "3"].map(Number));
show("builtin callback parseInt", ["1", "2", "3"].map(parseInt));
show("Boolean filter", [0, 1, "", "a", null].filter(Boolean));
class K { constructor() { this.x = 1; } }
err("class callback", () => [1].forEach(K));
err("map(K)", () => [1].map(K));
const callableProxy = new Proxy(function (x) { return x * 3; }, {});
show("callable proxy", [1, 2].map(callableProxy));
show("nested builtin as callback", [[1, 2], [3]].map(a => a.map(x => x + 1)));
show("builtin bound as callback", [[1, 2], [3]].map(Function.prototype.call.bind(Array.prototype.indexOf)));

// Not-callable callbacks: the check is after ToObject and the length read.
for (const v of [undefined, null, 1, "x", true, {}, [], Symbol("s")]) {
    err("forEach(" + (typeof v === "symbol" ? "symbol" : JSON.stringify(v)) + ")", () => [1].forEach(v));
}
err("map(undefined) on empty", () => [].map());
err("some(null)", () => [].some(null));
err("every(1)", () => [].every(1));
err("filter('f')", () => [].filter("f"));
err("find({})", () => [].find({}));
err("findIndex(true)", () => [].findIndex(true));
err("reduce(undefined)", () => [].reduce());
let lengthReads = 0;
const counted = { get length() { lengthReads++; return 0; } };
err("length read before callable check", () => Array.prototype.forEach.call(counted, 1));
console.log("length reads", lengthReads);

// Return values.
console.log("forEach returns", [1].forEach(x => x));
console.log("some on empty", [].some(() => true), "every on empty", [].every(() => false));
console.log("find none", [1].find(() => false), "findIndex none", [1].findIndex(() => false));
console.log("findIndex zero", [5].findIndex(x => x === 5));

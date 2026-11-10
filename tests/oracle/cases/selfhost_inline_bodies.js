// The self-hosted Array builtins' bodies and their callbacks' bodies inlined
// into a program's functions (selfhost_merge.cpp, inline_guarded.cpp): every
// section is a small function, so the inliner takes the builtin's copy and a
// literal callback into it. What each checks is that the copied bodies run
// exactly the builtin's algorithm with exactly that callback.

"use strict";
function show(label, v) { console.log(label, JSON.stringify(v)); }

function mapPlusK(a, k) { return a.map(x => x + k); }
function filterOdd(a) { return a.filter(x => (x & 1) === 1); }
function sumReduce(a) { return a.reduce((s, x) => s + x, 0); }
function sumNoInit(a) { return a.reduce((s, x) => s + x); }
function findBig(a, lim) { return a.find(x => x > lim); }
function findIdx(a, v) { return a.findIndex(x => x === v); }
function anyNeg(a) { return a.some(x => x < 0); }
function allPos(a) { return a.every(x => x > 0); }
function eachSum(a) { let t = 0; a.forEach(x => { t += x; }); return t; }
function callWith(a, f) { return a.map(f); }

for (let r = 0; r < 3; r++) {
    const a = [3, 1, 4, 1, 5, 9, 2, 6];
    show("map " + r, mapPlusK(a, r));
    show("filter " + r, filterOdd(a));
    show("reduce " + r, sumReduce(a) + sumNoInit(a));
    show("find " + r, [findBig(a, 4 + r), findBig(a, 100)]);
    show("findIndex " + r, [findIdx(a, 9), findIdx(a, -1)]);
    show("some/every " + r, [anyNeg(a), anyNeg([1, -1]), allPos(a), allPos([0])]);
    show("forEach " + r, eachSum(a));
}

// Values of every kind through the inlined bodies.
show("mixed", mapPlusK([1, "a", null, undefined, true, 1.5, -0, NaN], 1));
show("strings reduce", sumReduce(["a", "b", "c"]));
show("objects", filterOdd([{ valueOf() { return 3; } }, 2, "5", 7n === 7n ? 9 : 0]));
console.log("NaN find", findIdx([NaN], NaN), "minus zero", findIdx([-0], 0));

// A site whose callback is a parameter: one function, many callbacks.
const fs = [x => x * 2, x => -x, function (x) { return this === undefined; }, Math.abs, String];
for (const f of fs) show("param callback", callWith([1, -2, 3], f));
try { callWith([1], "nope"); } catch (e) { console.log("param non-callable", e.constructor.name, e.message); }

// Holes and array-likes through the same functions the plain arrays took.
show("holes map", mapPlusK([1, , 3], 1));
console.log("holes kept", 1 in mapPlusK([1, , 3], 1));
show("holes reduce", sumNoInit([, 1, , 2]));
show("holes find", findIdx([1, , 3], undefined));
const sub = class extends Array {}.from([5, 6]);
console.log("subclass through inlined site", mapPlusK(sub, 1).constructor === Array, mapPlusK(sub, 1).length);
const own = [1, 2];
own.map = () => "own";
console.log("own method through inlined site", mapPlusK(own, 1));
show("frozen through inlined site", mapPlusK(Object.freeze([1, 2]), 1));
try { sumNoInit([]); } catch (e) { console.log("reduce empty", e.constructor.name, e.message); }

// Mutation from the callback, inside an inlined walk.
function growing(a) { return a.map((x, i, arr) => { if (i < 3) arr.push(i); return x; }); }
show("growing", growing([7, 8]));
function shrinking(a) { return a.filter((x, i, arr) => { arr.length = 2; return true; }); }
show("shrinking", shrinking([1, 2, 3, 4]));
function becomesBoxed(a) { return a.map((x, i, arr) => { if (i === 0) arr.foo = "bar"; return x + 1; }); }
show("own prop mid-walk", becomesBoxed([1, 2, 3]));
function protoMidWalk(a) {
    return a.map((x, i, arr) => { if (i === 0) Object.setPrototypeOf(arr, { __proto__: Array.prototype }); return x; });
}
show("prototype set mid-walk", protoMidWalk([1, 2, 3]));
function deleteAhead(a) { let n = 0; a.forEach((x, i, arr) => { n++; delete arr[i + 1]; }); return n; }
console.log("delete ahead", deleteAhead([1, 2, 3, 4, 5]));

// The callback throws: the walk stops, and a stack names the callback and the builtin.
function thrower(a) { return a.map(x => { if (x === 2) throw new TypeError("two"); return x; }); }
try { thrower([1, 2, 3]); } catch (e) { console.log("thrown", e.constructor.name, e.message); }
function counted(a) {
    let seen = 0;
    try { a.forEach(x => { seen++; if (x > 1) throw 0; }); } catch (e) { return seen; }
    return -1;
}
console.log("seen before throw", counted([1, 2, 3]));

// Recursion through an inlined site.
function sumDeep(v) { return Array.isArray(v) ? v.reduce((s, x) => s + sumDeep(x), 0) : v; }
console.log("recursive", sumDeep([1, [2, [3, [4, [5]]]], [[6]]]));
function mapDeep(v) { return Array.isArray(v) ? v.map(mapDeep) : v * 10; }
show("recursive map", mapDeep([1, [2, [3]], 4]));

// Closures the callbacks make and capture survive the inlined bodies.
function makers(a) { return a.map(x => () => x * 2); }
show("closures", makers([1, 2, 3]).map(f => f()));
function captureLoop(a) {
    const out = [];
    for (let i = 0; i < 3; i++) out.push(a.filter(x => x > i).length);
    return out;
}
show("captured loop var", captureLoop([0, 1, 2, 3]));

// Hot: many calls so every site is warm.
let hot = 0;
for (let r = 0; r < 3000; r++) {
    const a = [r, r + 1, r + 2];
    hot += mapPlusK(a, 1)[2] + filterOdd(a).length + sumReduce(a) + sumNoInit(a);
    hot += findBig(a, r) + findIdx(a, r + 2) + (anyNeg(a) ? 1 : 0) + (allPos(a) ? 1 : 0) + eachSum(a);
}
console.log("hot", hot);

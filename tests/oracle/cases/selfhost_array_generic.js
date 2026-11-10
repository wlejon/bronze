// Array.prototype iteration builtins on receivers that are not arrays:
// array-likes, strings, typed arrays, proxies, getters, and the length's
// conversion (ToLength).
"use strict";

function show(label, v) { console.log(label, JSON.stringify(v)); }
function err(label, f) {
    try { f(); console.log(label, "no throw"); }
    catch (e) { console.log(label, e.constructor.name + ": " + e.message); }
}
const AP = Array.prototype;

const like = { length: 3, 0: "a", 2: "c", 3: "not seen" };
show("forEach array-like", (() => { const r = []; AP.forEach.call(like, (x, i) => r.push(i + x)); return r; })());
show("map array-like", AP.map.call(like, x => x + "!"));
console.log("map array-like is array", Array.isArray(AP.map.call(like, x => x)));
show("filter array-like", AP.filter.call(like, () => true));
console.log("some array-like", AP.some.call(like, x => x === "c"));
console.log("every array-like", AP.every.call(like, x => typeof x === "string"));
console.log("find array-like", AP.find.call(like, (x, i) => i === 1));
console.log("findIndex array-like", AP.findIndex.call(like, x => x === "c"));
console.log("reduce array-like", AP.reduce.call(like, (a, x) => a + x));
console.log("indexOf array-like", AP.indexOf.call(like, "c"), AP.indexOf.call(like, "not seen"));

// Strings: ToObject gives a String object, its indices are its characters.
show("map string", AP.map.call("abc", c => c.toUpperCase()));
console.log("indexOf string", AP.indexOf.call("hello", "l"));
console.log("reduce string", AP.reduce.call("abc", (a, c) => c + a));
console.log("every string", AP.every.call("aaa", c => c === "a"));

// Primitives with no indices.
console.log("forEach number", AP.forEach.call(5, () => { throw 1; }));
show("map boolean", AP.map.call(true, x => x));
console.log("indexOf symbol", AP.indexOf.call(Symbol("s"), 1));

// Typed arrays through .call (the generic algorithm, not %TypedArray%'s).
const ta = new Float64Array([1.5, 2.5, 3.5]);
show("map typed array via call", AP.map.call(ta, x => x * 2));
console.log("map typed array via call is array", Array.isArray(AP.map.call(ta, x => x)));
console.log("indexOf typed", AP.indexOf.call(new Int8Array([1, 2, 3]), 3));
console.log("reduce typed", AP.reduce.call(new Uint8Array([200, 100]), (a, x) => a + x));
show("filter typed", AP.filter.call(new Int16Array([-1, 2, -3]), x => x < 0));

// null and undefined receivers.
for (const name of ["forEach", "map", "filter", "some", "every", "find", "findIndex", "reduce"]) {
    err(name + ".call(null)", () => AP[name].call(null, () => 1));
    err(name + ".call(undefined)", () => AP[name].call(undefined, () => 1));
}

// ToLength of the length.
const lens = [-1, -0, 0.5, 2.9, "2", NaN, null, true, "0x2", { valueOf() { return 2; } }, [2]];
for (const len of lens) {
    const o = { length: len, 0: "x", 1: "y", 2: "z" };
    let n = 0;
    AP.forEach.call(o, () => n++);
    console.log("length", typeof len === "object" && len !== null ? "obj" : String(len), "->", n);
}
err("length symbol", () => AP.forEach.call({ length: Symbol() }, () => 1));
err("length bigint", () => AP.map.call({ length: 1n }, () => 1));
err("length valueOf throws", () => AP.some.call({ length: { valueOf() { throw new RangeError("len"); } } }, () => 1));
console.log("huge length with early exit", AP.some.call({ length: 2 ** 53 + 10, 0: 1 }, x => x === 1));
console.log("huge length findIndex", AP.findIndex.call({ length: Infinity, 5: "f" }, x => x === "f"));
console.log("indexOf huge", AP.indexOf.call({ length: 1e300, 7: "q" }, "q", 5));

// Getters: HasProperty, then Get, index by index, in order.
const log = [];
const gl = {
    get length() { log.push("length"); return 2; },
    get 0() { log.push("get0"); return "a"; },
    get 1() { log.push("get1"); return "b"; },
};
AP.forEach.call(gl, x => log.push("cb " + x));
show("getter order", log);
const withProto = [1, , 3];
Object.setPrototypeOf(withProto, Object.create(Array.prototype, { 1: { value: "inherited" } }));
show("hole filled from chain", withProto.map(x => x));
console.log("indexOf from chain", withProto.indexOf("inherited"));

// Proxies: every trap observed, in the specification's order.
function traced(target) {
    const t = [];
    const p = new Proxy(target, {
        get(o, k, r) { t.push("get " + String(k)); return Reflect.get(o, k, r); },
        has(o, k) { t.push("has " + String(k)); return Reflect.has(o, k); },
    });
    return [p, t];
}
{
    const [p, t] = traced([10, , 30]);
    AP.forEach.call(p, () => {});
    show("proxy forEach", t);
}
{
    const [p, t] = traced([10, 20]);
    AP.find.call(p, () => false);
    show("proxy find", t);
}
{
    const [p, t] = traced([10, 20, 30]);
    AP.indexOf.call(p, 20);
    show("proxy indexOf", t);
}
{
    const [p, t] = traced([10, 20]);
    AP.reduce.call(p, (a, x) => a + x);
    show("proxy reduce", t);
}
{
    const [p, t] = traced([1, 2]);
    const r = AP.map.call(p, x => x * 2);
    show("proxy map", t);
    show("proxy map result", r);
}
{
    const [p, t] = traced([1, 2]);
    AP.filter.call(p, x => x > 1);
    show("proxy filter", t);
}

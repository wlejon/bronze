// The self-hosted builtins are built-in function objects in every respect a
// program can observe: name, length, toString, no [[Construct]], no
// `prototype`, property attributes, identity, and their frame in Error.stack.
"use strict";

function err(label, f) {
    try { f(); console.log(label, "no throw"); }
    catch (e) { console.log(label, e.constructor.name); }
}

const fns = [
    ["forEach", Array.prototype.forEach], ["map", Array.prototype.map],
    ["filter", Array.prototype.filter], ["some", Array.prototype.some],
    ["every", Array.prototype.every], ["find", Array.prototype.find],
    ["findIndex", Array.prototype.findIndex], ["reduce", Array.prototype.reduce],
];
for (const [label, f] of fns) {
    console.log(label, typeof f, f.name, f.length, String(f), Function.prototype.toString.call(f));
    console.log("  own keys", JSON.stringify(Reflect.ownKeys(f)), "prototype" in f, f.hasOwnProperty("prototype"));
    const nd = Object.getOwnPropertyDescriptor(f, "name");
    const ld = Object.getOwnPropertyDescriptor(f, "length");
    console.log("  name attrs", nd.writable, nd.enumerable, nd.configurable, "length attrs", ld.writable, ld.enumerable, ld.configurable);
    err("  new", () => new f());
    err("  Reflect.construct", () => Reflect.construct(f, []));
    console.log("  proto is Function.prototype", Object.getPrototypeOf(f) === Function.prototype);
    console.log("  extensible", Object.isExtensible(f));
}
for (const name of ["forEach", "map", "filter", "some", "every", "find", "findIndex", "reduce", "indexOf"]) {
    const d = Object.getOwnPropertyDescriptor(Array.prototype, name);
    console.log(name, "on Array.prototype", d.writable, d.enumerable, d.configurable, d.value === [][name]);
}
const ad = Object.getOwnPropertyDescriptor(Object, "assign");
console.log("Object.assign attrs", ad.writable, ad.configurable);
console.log("Array.prototype keys hide them", Object.keys(Array.prototype).length);
for (const k in []) console.log("for-in saw", k);

// The same object however it is reached.
console.log("identity", [].map === Array.prototype.map, [].map === [1].map, Object.assign === Object["assign"]);
const sub = new (class extends Array {})();
console.log("identity via subclass", sub.forEach === [].forEach);
console.log("bind", [1, 2].map.bind([3, 4])(x => x * 2).join());
console.log("bind name", Array.prototype.map.bind(null).name);
console.log("call", Array.prototype.map.call([5], x => x + 1)[0]);
console.log("apply", Array.prototype.indexOf.apply([7, 8], [8]));
console.log("Reflect.apply", Reflect.apply(Array.prototype.some, [1], [x => x === 1]));
console.log("as a value", [[1, 2], [3]].map(Function.prototype.call, Array.prototype.indexOf).join());

// Properties added to the function objects are kept.
console.log("expando", [].map.custom);
delete Array.prototype.map.custom;
console.log("expando deleted", [].map.custom);

// Error.stack: the builtin's own frame, under its V8 name, between the
// callback and its caller, and none of the library's helpers.
function frames(f) {
    try { f(); } catch (e) {
        return e.stack.split("\n").slice(1).map(l => l.trim())
            .filter(l => l.endsWith("(<anonymous>)")).join(" | ");
    }
    return "no throw";
}
const boom = () => { throw new Error("boom"); };
console.log(frames(() => [1].forEach(boom)));
console.log(frames(() => [1].map(boom)));
console.log(frames(() => [1].filter(boom)));
console.log(frames(() => [1].some(boom)));
console.log(frames(() => [1].every(boom)));
console.log(frames(() => [1].find(boom)));
console.log(frames(() => [1].findIndex(boom)));
console.log(frames(() => [1].reduce(boom, 0)));
console.log(frames(() => [[1]].forEach(a => a.map(boom))));
try { [1].forEach(boom); } catch (e) {
    console.log("stack starts", e.stack.split("\n")[0]);
    console.log("helpers hidden", !/toLength|toIntegerOrInfinity|builtins\.js|selfhost/.test(e.stack));
}

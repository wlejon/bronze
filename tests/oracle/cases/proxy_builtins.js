// Every builtin that reaches a Proxy's internal methods, with the trap order
// each one produces — so a handler that logs its traps sees the same sequence
// under bronze as under V8. The expected output is node's.
//
// 10.5 gives a proxy thirteen internal methods, and the standard library
// reaches every one of them from somewhere other than a property access:
// `Object.defineProperty` is [[DefineOwnProperty]], `Object.keys` is
// [[OwnPropertyKeys]] filtered by [[GetOwnProperty]], `Object.freeze` is
// [[PreventExtensions]] followed by a [[DefineOwnProperty]] per key, `new`
// is [[Construct]], a `for-in` walks [[GetPrototypeOf]]. Each has to route
// through the trap — with the 10.5 invariants checked on its answer — rather
// than through the target's storage, or the proxy is bypassed exactly where
// a membrane or a mock needs it not to be.
//
// The last two sections are the receiver rules and revocation: a proxy that
// sits in a prototype chain sees `Object.create(proxy)`'s instance as the
// receiver of `get`/`set`, and a revoked proxy answers every internal method
// with the same TypeError (10.5, "revoked" in each trap's step 3).
const log = [];
const S = (k) => (typeof k === "symbol" ? k.toString() : String(k));
function logged(target, extra) {
    const h = {
        get(t, k, r) { log.push("get:" + S(k)); return Reflect.get(t, k, r); },
        set(t, k, v, r) { log.push("set:" + S(k)); return Reflect.set(t, k, v, r); },
        has(t, k) { log.push("has:" + S(k)); return Reflect.has(t, k); },
        deleteProperty(t, k) { log.push("del:" + S(k)); return Reflect.deleteProperty(t, k); },
        ownKeys(t) { log.push("ownKeys"); return Reflect.ownKeys(t); },
        getOwnPropertyDescriptor(t, k) { log.push("gopd:" + S(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
        defineProperty(t, k, d) { log.push("def:" + S(k) + ":" + Object.keys(d).sort().join("/")); return Reflect.defineProperty(t, k, d); },
        getPrototypeOf(t) { log.push("gpo"); return Reflect.getPrototypeOf(t); },
        setPrototypeOf(t, p) { log.push("spo"); return Reflect.setPrototypeOf(t, p); },
        isExtensible(t) { log.push("isExt"); return Reflect.isExtensible(t); },
        preventExtensions(t) { log.push("prevExt"); return Reflect.preventExtensions(t); },
        apply(t, th, args) { log.push("apply:" + args.join("|")); return Reflect.apply(t, th, args); },
        construct(t, args, nt) { log.push("construct:" + args.join("|") + ":" + (nt === p ? "nt=proxy" : nt.name)); return Reflect.construct(t, args, nt); },
    };
    const p = new Proxy(target, Object.assign(h, extra || {}));
    return p;
}
function show(label, fn) {
    log.length = 0;
    let r;
    try { r = fn(); } catch (e) { r = "throws " + e.constructor.name + ": " + e.message; }
    console.log(label, "=>", typeof r === "string" ? r : JSON.stringify(r), "|", log.join(","));
}

// Descriptors and the prototype link.
let p = logged({ a: 1, b: 2 });
show("defineProperty", () => Object.defineProperty(p, "c", { value: 3, enumerable: true, configurable: true, writable: true }) === p);
show("defineProperty accessor", () => Object.defineProperty(p, "d", { get() { return 4; }, configurable: true }) === p);
show("defineProperties", () => Object.defineProperties(p, { e: { value: 5 } }) === p);
show("getOwnPropertyDescriptor", () => Object.getOwnPropertyDescriptor(p, "a"));
show("getOwnPropertyDescriptor missing", () => Object.getOwnPropertyDescriptor(p, "zz"));
show("getOwnPropertyDescriptors", () => Object.keys(Object.getOwnPropertyDescriptors(p)));
show("getPrototypeOf", () => Object.getPrototypeOf(p) === Object.prototype);
show("setPrototypeOf", () => Object.setPrototypeOf(p, null) === p);
show("getPrototypeOf after", () => Object.getPrototypeOf(p));
show("Reflect.getPrototypeOf", () => Reflect.getPrototypeOf(p));
show("Reflect.setPrototypeOf", () => Reflect.setPrototypeOf(p, Object.prototype));
show("isPrototypeOf", () => Object.prototype.isPrototypeOf(p));
show("instanceof", () => p instanceof Object);

// Key enumeration, membership, deletion, Reflect, and the integrity levels.
p = logged({ a: 1, b: 2, [Symbol("s")]: 3 });
Object.defineProperty(p, "hidden", { value: 0, enumerable: false });
log.length = 0;
show("keys", () => Object.keys(p));
show("values", () => Object.values(p));
show("entries", () => Object.entries(p));
show("getOwnPropertyNames", () => Object.getOwnPropertyNames(p));
show("getOwnPropertySymbols", () => Object.getOwnPropertySymbols(p).length);
show("Reflect.ownKeys", () => Reflect.ownKeys(p).length);
show("JSON.stringify", () => JSON.stringify(p));
show("JSON.stringify nested", () => JSON.stringify({ x: p }));
show("for-in", () => { const r = []; for (const k in p) r.push(k); return r; });
show("spread", () => ({ ...p }));
show("Object.assign from", () => Object.assign({}, p));
show("Object.assign to", () => Object.assign(p, { q: 1 }) === p);
show("hasOwnProperty", () => Object.prototype.hasOwnProperty.call(p, "a"));
show("Object.hasOwn", () => Object.hasOwn(p, "a"));
show("propertyIsEnumerable", () => Object.prototype.propertyIsEnumerable.call(p, "hidden"));
show("in", () => "a" in p);
show("in missing", () => "nope" in p);
show("delete", () => delete p.a);
show("delete then in", () => "a" in p);
show("Reflect.has", () => Reflect.has(p, "b"));
show("Reflect.get", () => Reflect.get(p, "b"));
show("Reflect.set", () => Reflect.set(p, "b", 20));
show("Reflect.deleteProperty", () => Reflect.deleteProperty(p, "b"));
show("Reflect.defineProperty", () => Reflect.defineProperty(p, "r", { value: 1, configurable: true }));
show("Reflect.getOwnPropertyDescriptor", () => Reflect.getOwnPropertyDescriptor(p, "r").value);
show("Reflect.isExtensible", () => Reflect.isExtensible(p));
show("Object.isExtensible", () => Object.isExtensible(p));
show("Object.isFrozen", () => Object.isFrozen(p));
show("Object.isSealed", () => Object.isSealed(p));
show("Object.preventExtensions", () => Object.preventExtensions(p) === p);
show("Reflect.preventExtensions", () => Reflect.preventExtensions(p));
show("isExtensible after", () => Object.isExtensible(p));
show("isFrozen after prevent", () => Object.isFrozen(p));
p = logged({ a: 1 });
show("Object.seal", () => Object.seal(p) === p);
show("isSealed after", () => Object.isSealed(p));
p = logged({ a: 1, b: 2 });
show("Object.freeze", () => Object.freeze(p) === p);
show("isFrozen after freeze", () => Object.isFrozen(p));
show("set on frozen", () => { "use strict"; try { p.a = 9; return "no"; } catch (e) { return e.constructor.name; } });
show("Object.fromEntries of proxy pairs", () => Object.fromEntries(logged([["k", "v"]])));
show("Array.isArray", () => Array.isArray(logged([])));
show("Object.prototype.toString", () => Object.prototype.toString.call(logged([])));
show("Object.prototype.toString fn", () => Object.prototype.toString.call(logged(function () {})));
show("structured keys of empty", () => Object.keys(logged({})));

// Iteration: GetIterator reads `[Symbol.iterator]` through [[Get]], and the
// ArrayIterator it returns is generic — `length` on every step, then the
// element — so a proxy over an array iterates through its `get` trap.
show("array spread", () => [...logged([1, 2])]);
show("for-of", () => { const r = []; for (const v of logged(["x", "y"])) r.push(v); return r; });
show("Array.from", () => Array.from(logged([3])));
show("destructure", () => { const [a, b] = logged([5, 6]); return a + b; });
show("proxied iterator", () => {
    const it = logged({ i: 0, next() { return this.i < 2 ? { value: this.i++, done: false } : { value: undefined, done: true }; }, [Symbol.iterator]() { return this; } });
    return [...it];
});

// Call and construct.
function target(a, b) { return "t:" + this?.tag + ":" + a + "," + b; }
function Ctor(x) { this.x = x; }
Ctor.prototype.hi = function () { return "hi" + this.x; };
p = logged(target);
show("call", () => p(1, 2));
show(".call", () => p.call({ tag: "T" }, 3, 4));
show(".apply", () => p.apply({ tag: "A" }, [5, 6]));
show("Reflect.apply", () => Reflect.apply(p, { tag: "R" }, [7]));
show("bind", () => p.bind({ tag: "B" }, 8)(9));
show("spread call", () => p(...[10, 11]));
show("method call", () => ({ tag: "M", m: p }).m(12));
show("typeof", () => typeof p);
show("Function.prototype.toString", () => typeof Function.prototype.toString.call(p));
show("name/length via get", () => [p.name, p.length]);
p = logged(Ctor);
show("new", () => new p(1).hi());
show("Reflect.construct", () => Reflect.construct(p, [2]).hi());
show("Reflect.construct newTarget", () => { function NT() {} NT.prototype = { hi() { return "nt"; } }; return Reflect.construct(p, [3], NT).hi(); });
show("new spread", () => new p(...[4]).x);
show("instanceof through proxy ctor", () => new Ctor(0) instanceof p);
show("class extends proxy", () => { class D extends p { constructor() { super(5); } } return new D().hi(); });
show("class extends proxy, spread super", () => { class D extends p { constructor(...a) { super(...a); } } return new D(6).hi(); });
show("construct non-ctor", () => { try { new (logged(() => 1))(); return "no"; } catch (e) { return e.constructor.name; } });
show("call non-callable", () => { try { logged({})(); return "no"; } catch (e) { return e.constructor.name; } });
show("apply trap returns", () => new Proxy(function () {}, { apply: () => "trapped" })());
show("construct trap returns", () => new (new Proxy(function () {}, { construct: () => ({ made: true }) }))().made);
show("construct trap non-object", () => { try { new (new Proxy(function () {}, { construct: () => 1 }))(); return "no"; } catch (e) { return e.constructor.name; } });

// Proxy in the prototype chain: receiver semantics.
p = logged({ inherited: "iv", set acc(v) { this.viaSetter = v; } });
const child = Object.create(p);
show("proto get", () => child.inherited);
show("proto get receiver", () => { const q = new Proxy({}, { get(t, k, r) { return r === child ? "recv=child" : "recv=other"; } }); return Object.create(q).x; });
show("proto has", () => "inherited" in child);
show("proto has own miss", () => "own" in child);
show("proto set", () => { child.newProp = 1; return Object.keys(child); });
show("proto set receiver", () => { const q = new Proxy({}, { set(t, k, v, r) { Reflect.defineProperty(r, "stamped", { value: k, configurable: true }); return true; } }); const c = Object.create(q); c.z = 1; return [c.stamped, Object.hasOwn(c, "z")]; });
show("proto set accessor", () => { child.acc = 7; return [child.viaSetter, Object.hasOwn(child, "viaSetter")]; });
show("proto for-in", () => { const r = []; for (const k in child) r.push(k); return r; });
show("proto hasOwnProperty", () => Object.hasOwn(child, "inherited"));
show("proto getPrototypeOf chain", () => Object.getPrototypeOf(child) === p);
show("proto instanceof", () => { function F() {} F.prototype = p; return child instanceof F; });
show("proto method call", () => { const q = new Proxy({ m() { return this === c2 ? "this=child" : "this=other"; } }, {}); const c2 = Object.create(q); return c2.m(); });
show("proto delete miss", () => delete child.inherited);

// Revocation.
show("revocable", () => { const r = Proxy.revocable({ a: 1 }, {}); const before = r.proxy.a; r.revoke(); r.revoke(); return [before, typeof r.revoke, r.revoke.name, r.revoke.length, Object.keys(r)]; });
const rv = Proxy.revocable(function f() {}, {});
rv.revoke();
const ops = {
    get: () => rv.proxy.x, set: () => { rv.proxy.x = 1; }, has: () => "x" in rv.proxy, delete: () => delete rv.proxy.x,
    keys: () => Object.keys(rv.proxy), gopd: () => Object.getOwnPropertyDescriptor(rv.proxy, "x"),
    define: () => Object.defineProperty(rv.proxy, "x", { value: 1 }), gpo: () => Object.getPrototypeOf(rv.proxy),
    spo: () => Object.setPrototypeOf(rv.proxy, null), isExt: () => Object.isExtensible(rv.proxy),
    prevExt: () => Object.preventExtensions(rv.proxy), call: () => rv.proxy(), construct: () => new rv.proxy(),
    apply: () => Reflect.apply(rv.proxy, null, []), stringify: () => JSON.stringify(rv.proxy), spread: () => ({ ...rv.proxy }),
    forin: () => { for (const k in rv.proxy) {} }, freeze: () => Object.freeze(rv.proxy), isFrozen: () => Object.isFrozen(rv.proxy),
    typeof: () => typeof rv.proxy, isArray: () => Array.isArray(rv.proxy), ownKeys: () => Reflect.ownKeys(rv.proxy),
    proxyOfRevoked: () => new Proxy(rv.proxy, {}), handlerRevoked: () => new Proxy({}, rv.proxy),
};
for (const [name, op] of Object.entries(ops)) {
    let r;
    try { r = op(); r = "ok:" + String(r); } catch (e) { r = e.constructor.name + ": " + e.message.slice(0, 40); }
    console.log("revoked", name, "=>", r);
}
show("isArray revoked proxy of array", () => { const r = Proxy.revocable([], {}); r.revoke(); try { return Array.isArray(r.proxy); } catch (e) { return e.constructor.name; } });

// Object spread and object rest between plain objects take the source's
// layout instead of looking each key up (runtime/spread_plain.cpp). Every
// way a source or a target can fall outside that case has to give the same
// answer as the general walk.
const log = [];
function show(label, o) {
    const keys = Reflect.ownKeys(o).map((k) => (typeof k === "symbol" ? k.toString() : k));
    const parts = [];
    for (const k of Reflect.ownKeys(o)) {
        const d = Object.getOwnPropertyDescriptor(o, k);
        const flags = (d.enumerable ? "e" : "-") + (d.writable ? "w" : "-") + (d.configurable ? "c" : "-");
        const v = "value" in d ? (typeof d.value === "object" && d.value !== null ? JSON.stringify(d.value) : String(d.value)) : "accessor";
        parts.push((typeof k === "symbol" ? k.toString() : k) + "=" + v + ":" + flags);
    }
    console.log(label + " [" + keys.join(",") + "] " + parts.join(" "));
}

// Plain to plain: order, overriding, an empty target and one with keys.
const defaults = { color: 1, alpha: 0.5, texture: "white", matrix: null };
const options = { alpha: 0.25, width: 3, color: "red" };
show("copy", { ...defaults });
show("override", { ...defaults, ...options });
show("after-own", { first: 0, alpha: 9, ...defaults, ...options, last: true });
show("twice", { ...defaults, ...defaults });

// The same layouts again and again, with values of other kinds: the remembered
// shape must not decide what a slot can hold.
for (let i = 0; i < 6; i++) {
    const src = { x: i % 2 ? i + 0.5 : "s" + i, y: i % 3 ? { n: i } : i, z: i };
    const out = { ...defaults, ...src };
    const again = { ...src };
    out.x = i;
    again.z = "z";
    if (i > 3) show("loop" + i, { out, again, src });
}

// Getters on the source run, once each and in order, and land as data.
const withGetters = {
    a: 1,
    get b() { log.push("get b"); return 2; },
    c: 3,
    get d() { log.push("get d"); return this.a + 10; },
};
show("getters", { z: 0, ...withGetters });
console.log(log.join("|"));
log.length = 0;

// Index-like keys first and ascending, then names in insertion order.
const mixed = { b: 1, 10: "ten", a: 2, 2: "two", "-1": "neg", 1.5: "frac", 0: "zero" };
show("indices", { ...mixed });
show("indices-into", { q: 1, 5: "five", ...mixed });

// Symbols are copied, after the strings; non-enumerable keys are not.
const sym = Symbol("s");
const hidden = Symbol("hidden");
const symSource = { p: 1, [sym]: "sym", q: 2 };
Object.defineProperty(symSource, "skipped", { value: 1, enumerable: false });
Object.defineProperty(symSource, hidden, { value: 1, enumerable: false });
Object.defineProperty(symSource, "frozenLike", { value: 7, enumerable: true, writable: false, configurable: false });
show("symbols", { ...symSource });

// A dictionary-mode source (a delete), a null-prototype source, a frozen one.
const dict = { a: 1, b: 2, c: 3, d: 4 };
delete dict.b;
dict.e = 5;
show("dictionary", { ...dict });
const bare = Object.create(null);
bare.x = 1;
bare.y = 2;
show("null-proto", { ...bare });
show("frozen", { ...Object.freeze({ f: 1, g: 2 }) });
class Point { constructor() { this.x = 1; this.y = 2; } get len() { return 3; } }
show("instance", { ...new Point() });

// A target that is not at the start of the source's layout.
const proto = { inherited: 1 };
show("proto-literal", { __proto__: proto, own: 1, ...defaults });
show("accessor-target", { get alpha() { return "getter"; }, color: 0, ...defaults });

// Sources that are not plain objects.
show("array", { ...["a", , "c"] });
show("string", { ..."hi" });
show("null", { ...null });
show("undefined", { ...undefined });
show("number", { ...42 });
function withStatic() {}
withStatic.stat = 1;
show("function", { ...withStatic });
const arrWithName = [1, 2];
arrWithName.extra = "named";
show("array-named", { ...arrWithName });
show("typed", { ...new Uint8Array([7, 8]) });

// More keys than an object holds inline, and more than the walk gathers.
const wide = {};
for (let i = 0; i < 20; i++) wide["k" + i] = i;
show("wide", { ...wide });
show("wide-again", { pre: 1, ...wide, k3: "over" });
const huge = {};
for (let i = 0; i < 60; i++) huge["h" + i] = i;
console.log(Object.keys({ ...huge }).length, Object.keys({ ...huge, ...wide }).length);

// Rest: the named keys are left out and their getters are not run.
const { color, ...restA } = defaults;
show("rest", restA);
const { a: ra, ...restGetters } = withGetters;
show("rest-getters", restGetters);
console.log(log.join("|"));
log.length = 0;
const { b: rb, d: rd, ...restSkips } = withGetters;
show("rest-skips", restSkips);
console.log(log.join("|"));
const { [sym]: rs, p, ...restSym } = symSource;
show("rest-symbols", restSym);
const { 2: two, a: ma, ...restMixed } = mixed;
show("rest-indices", restMixed);
const { c: dc, ...restDict } = dict;
show("rest-dictionary", restDict);
const { 0: h, ...restString } = "hey";
show("rest-string", restString);
const { 1: second, length, ...restArray } = ["a", "b", "c"];
show("rest-array", restArray);
const { ...restNumber } = 5;
show("rest-number", restNumber);
const { k1, k19, ...restWide } = wide;
show("rest-wide", restWide);
for (let i = 0; i < 4; i++) {
    const { x, ...others } = { x: i, y: i % 2 ? "odd" : i / 2, z: [i] };
    const { y, ...others2 } = { x: i, y: i, z: i };
    if (i > 1) show("rest-loop" + i, { others, others2 });
}
const viaProxy = new Proxy({ a: 1, b: 2, c: 3 }, {
    ownKeys(t) { log.length = 0; log.push("ownKeys"); return Reflect.ownKeys(t); },
    getOwnPropertyDescriptor(t, k) { log.push("gopd " + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
    get(t, k) { log.push("get " + String(k)); return t[k]; },
});
const { b: pb, ...restProxy } = viaProxy;
show("rest-proxy", restProxy);
console.log(log.join("|"));
show("spread-proxy", { ...viaProxy });
console.log(log.join("|"));

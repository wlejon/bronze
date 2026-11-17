// super() enters a base compiled code may call directly (its body reads no
// new.target and calls no super of its own) without the runtime, and every
// other base takes the runtime's path: the base's [[Prototype]] link changed
// under the class, a base that reads new.target, a proxy, a bound function,
// a native base, `extends null`, and a base that throws. A vetted `new` of a
// derived class repeats through the runtime's short path.
function Plain(a, b) { this.a = a; this.b = b; }
function ReturnsObject() { this.lost = true; return { replaced: 1 }; }
function ReturnsPrimitive() { this.kept = true; return 7; }
function ReadsTarget() { this.target = new.target && new.target.name; }
function Throws(x) { if (x) throw new Error('base threw ' + x); this.ok = 1; }
class Base { constructor(v) { this.v = v; } }

class D1 extends Plain { constructor(a) { super(a); this.c = 3; } }
class D2 extends ReturnsObject { constructor() { super(); this.after = 2; } }
class D3 extends ReturnsPrimitive { constructor() { super(); this.after = 3; } }
class D4 extends ReadsTarget { constructor() { super(); } }
class D5 extends Throws { constructor(x) { super(x); } }
class D6 extends Base { constructor() { super(6, 'extra'); } }
class Mid extends Base { constructor(v) { super(v + 1); this.mid = true; } }
class D7 extends Mid { constructor() { super(10); } }

function show(o) { return JSON.stringify(o) + ' ' + Object.getPrototypeOf(o).constructor.name; }

for (let i = 0; i < 3; i++) {
    console.log(show(new D1(i)));
    console.log(show(new D2()));
    console.log(show(new D3()));
    console.log(show(new D4()));
    console.log(show(new D6()));
    console.log(show(new D7()));
    try { console.log(show(new D5(i))); } catch (e) { console.log(e.message); }
}

// The link changed: the next super() constructs the new base.
class Swap extends Plain { constructor() { super(1, 2); } }
console.log(show(new Swap()));
Object.setPrototypeOf(Swap, ReadsTarget);
console.log(show(new Swap()));
Object.setPrototypeOf(Swap, new Proxy(Plain, {
    construct(t, args, nt) { const o = Reflect.construct(t, args, nt); o.proxied = true; return o; },
}));
console.log(show(new Swap()));
Object.setPrototypeOf(Swap, Plain.bind(null, 'bound'));
console.log(show(new Swap()));
Object.setPrototypeOf(Swap, Map);
try { console.log(new Swap() instanceof Map); } catch (e) { console.log(e.constructor.name); }
Object.setPrototypeOf(Swap, Function.prototype);
try { new Swap(); } catch (e) { console.log(e.constructor.name); }
Object.setPrototypeOf(Swap, Plain);
console.log(show(new Swap()));

class N extends null { constructor() { super(); } }
try { new N(); } catch (e) { console.log(e.constructor.name); }

class Twice extends Plain { constructor() { super(1); super(2); } }
try { new Twice(); } catch (e) { console.log(e.constructor.name); }

class M extends Map { constructor() { super([[1, 2]]); this.extra = 1; } }
const m = new M();
console.log(m.get(1), m.size, m.extra, m instanceof Map);

// A vetted derived class whose prototype is reassigned between `new`s.
class R extends Plain { constructor() { super('r'); } }
new R();
R.prototype = { tag: 'new proto' };
const r = new R();
console.log(r.a, r.tag, r instanceof R);

// `this.link.payload`: `link` has no type (written from outside the class),
// and `payload` is declared by one class subtree only, so the read off it is
// guessed to be of that subtree and guarded by its family (lower_key_owner_
// guess.cpp). Every receiver here that is NOT of the family must take the
// generic read and give the same answer; the ones that are must read the slot.

class Node {
    constructor(name) {
        this.name = name;
        this.payload = 'payload:' + name;
        this.link = null;
    }
    linked() { return this.link === null ? 'none' : this.link.payload; }
    setLinked(v) { if (this.link !== null) this.link.payload = v; }
}
class Leaf extends Node {
    constructor(name) { super(name); this.extra = 1; }
}
class Late extends Node {
    constructor(name) { super(name); }
    grow() { this.more = 2; }
}

const out = [];
const a = new Node('a'), b = new Leaf('b'), c = new Late('c');
c.grow();
const targets = [
    a, b, c,
    { payload: 'plain' },
    { name: 'x', payload: 'same-order', link: null },
    Object.create(a),
    Object.create({ get payload() { return 'getter'; } }),
    new (function OC() { this.payload = 'other-ctor'; })(),
    'a string', 42, true, [1, 2], function f() {}, Object.assign(Object.create(null), { payload: 'nullproto' }),
];
for (let rep = 0; rep < 3; rep++) {
    for (const t of targets) {
        a.link = t;
        let v;
        try { v = a.linked(); } catch (e) { v = 'threw ' + e.constructor.name; }
        out.push(typeof v === 'function' ? 'fn' : String(v));
    }
}
console.log(out.join(' | '));

// A family member whose payload was deleted, redefined as an accessor, or
// frozen: its shape changes, so the guard must not match it.
const d = new Leaf('d');
a.link = d;
console.log(a.linked());
delete d.payload;
console.log(a.linked());
Object.defineProperty(d, 'payload', { get() { return 'redefined'; }, configurable: true });
console.log(a.linked());
const e = new Node('e');
Object.freeze(e);
a.link = e;
console.log(a.linked());
try { a.setLinked('write-frozen'); } catch (err) { console.log('frozen', err instanceof TypeError); }
console.log(e.payload);
const g = new Leaf('g');
a.link = g;
a.setLinked('write-leaf');
console.log(g.payload, a.linked());
Object.defineProperty(g, 'payload', { writable: false });
try { a.setLinked('write-readonly'); } catch (err) { console.log('readonly', err instanceof TypeError); }
console.log(g.payload);

// Prototype payload shadowed and not.
const h = new Node('h');
Node.prototype.payload = 'proto';
delete h.payload;
a.link = h;
console.log(a.linked());

// The null and undefined receivers throw TypeError from the read.
a.link = undefined;
try { a.linked(); } catch (err) { console.log('undefined', err instanceof TypeError); }

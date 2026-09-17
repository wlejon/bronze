// A static field initializer that names its own class. 15.7.14 initializes
// the class binding (step 28) before it evaluates the static elements (step
// 33), and that binding lives in the CLASS scope — not the outer one a
// declaration makes, which is still in its dead zone until the definition
// has been evaluated. bronze opened the class scope only for private names, a
// static block or a named class expression, so a declaration's static
// initializer resolved the name outward and found the dead zone:
// `class C { static inst = new C(); }` was a ReferenceError at top level, in a
// block and in a function alike, while `static { C.inst = new C(); }` worked.

class Vec {
    constructor(x, y) { this.x = x; this.y = y; }
    static ZERO = new Vec(0, 0);
    static UNIT = Object.freeze(new Vec(1, 1));
    static count = Vec.ZERO.x + Vec.UNIT.x + 1;
    static make = (x, y) => new Vec(x, y);
    static name2 = Vec.name + typeof Vec;
}
console.log(Vec.ZERO instanceof Vec, Vec.UNIT.y, Vec.count, Vec.make(3, 4).y, Vec.name2);

{
    class Block { static self = Block; static isSelf = Block.self === Block; }
    console.log(Block.isSelf);
}

function factory() {
    class Local { static n = 1; static m = Local.n + 1; static nested = class { static outer = Local; }; }
    return Local.m + (Local.nested.outer === Local ? 1 : 0);
}
console.log(factory(), factory());

// A computed key and a static block interleave with the initializers in
// definition order, all seeing the binding.
const order = [];
class Order {
    static a = order.push('a') && Order.name;
    static { order.push('block:' + Order.a); }
    static ['b' + 1] = order.push('b') && Order.a;
}
console.log(order.join(','), Order.b1);

// The inner binding is immutable: a method that names the class keeps the
// class even after the outer declaration is reassigned.
let Ref = class Named { static self = () => Named; };
const keep = Ref;
Ref = null;
console.log(keep.self() === keep);

// The heritage is still read in the dead zone.
try {
    class Loop extends Loop { static x = 1; }
    console.log('extends self: no throw');
} catch (e) {
    console.log('extends self:', e.name);
}

// Derived classes: the binding is the subclass, and super's statics are
// reachable through it.
class Base { static tag = 'base'; }
class Derived extends Base { static tag = Derived.name + ':' + super.tag; static viaSelf = Derived.tag; }
console.log(Derived.tag, Derived.viaSelf, Base.tag);

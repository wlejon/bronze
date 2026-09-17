// `super.x` in a STATIC element. 13.3.7.1 starts the lookup at the home
// object's [[Prototype]], and the home object of a static method, accessor,
// field initializer or block is the constructor — whose [[Prototype]] is the
// heritage itself. bronze resolved every `super.x` against the heritage's
// `prototype`, which is right for an instance element and wrong for a static
// one: `static m() { return super.m(); }` read `Base.prototype.m`, found
// nothing, and threw "undefined is not a function".

class Base {
    static tag = 'base';
    static count = 0;
    static make(n) { return `Base.make(${n})`; }
    static get kind() { return 'kind of ' + this.name; }
    static set kind(v) { this.setKind = v; }
    m() { return 'Base#m'; }
}

class Derived extends Base {
    // A static method: the base's static, with `this` the subclass.
    static make(n) { return 'Derived>' + super.make(n) + ' on ' + this.name; }
    // A static getter through super runs with `this` = the receiver.
    static get kind() { return '[' + super.kind + ']'; }
    // A static field initializer.
    static tag = super.tag + '+derived';
    static built = super.make(1);
    // A static block.
    static { Derived.blockSaw = super.tag; }
    // A write through super lands on the RECEIVER (the subclass), not the base.
    static bump() { super.count = (super.count ?? 0) + 10; return [Base.count, Derived.count]; }
    // A setter through super runs with `this` = the subclass.
    static setKindVia(v) { super.kind = v; return [Base.setKind, Derived.setKind]; }
    // An instance element still starts at `Base.prototype`.
    m() { return 'Derived>' + super.m(); }
    inst = super.m();
}

console.log(Derived.make(2));
console.log(Derived.kind);
console.log(Derived.tag, Base.tag);
console.log(Derived.built, Derived.blockSaw);
console.log(Derived.bump().join(','));
console.log(Derived.setKindVia('k').join(','));
console.log(new Derived().m(), new Derived().inst);

// Two levels: super in a static reaches the nearest heritage, which reaches
// its own.
class Third extends Derived {
    static make(n) { return 'Third>' + super.make(n); }
    static tag = super.tag + '+third';
}
console.log(Third.make(3));
console.log(Third.tag);

// An arrow inside a static element keeps its `super`.
class Arrow extends Base {
    static viaArrow = (() => super.make('arrow'))();
    static later() { return () => super.tag; }
}
console.log(Arrow.viaArrow, Arrow.later()());

// A subclass of an intrinsic: super reaches the intrinsic's own statics.
class List extends Array {
    static of(...xs) { return 'List.of:' + super.of(...xs).length; }
    static from(xs) { return super.from(xs).map((x) => x * 2); }
}
console.log(List.of(1, 2, 3), List.from([1, 2]).join(','));

// A static named for what Function.prototype has: `super.call` is
// Function.prototype.call, reached through the base function.
class Callable extends Base {
    static invoke() { return typeof super.call; }
}
console.log(Callable.invoke());

// An absent static reads undefined, as any missing property does.
class Missing extends Base {
    static probe() { return super.nothing; }
}
console.log(Missing.probe());

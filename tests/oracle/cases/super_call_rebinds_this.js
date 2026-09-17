// `super()` binds the receiver (ECMA-262 13.3.7.1 step 7): when the base
// constructor returns an object, that object IS `this` from then on — every
// later read, the field initializers, the arrows that captured it, and the
// value `new` hands back. bronze once ran the derived body on the instance it
// had allocated and threw the base's object away.

class Swap { constructor(tag) { return { tag, fromBase: true }; } }

class Plain extends Swap {
    constructor() { super("plain"); this.d = 1; }
}
const p = new Plain();
console.log("plain:", p.fromBase, p.tag, p.d, p instanceof Plain, p instanceof Swap);

// Field initializers run on the rebound receiver.
class Fields extends Swap {
    f = 7;
    constructor() { super("fields"); }
}
const f = new Fields();
console.log("fields:", f.f, f.tag, f.fromBase);

// An arrow created BEFORE super() reads the receiver through the record, so
// it sees the rebinding too; one created after reads the same slot.
class Arrows extends Swap {
    constructor() {
        const before = () => this.tag;
        super("arrows");
        const after = () => this.tag;
        this.r = [before(), after()];
    }
}
console.log("arrows:", new Arrows().r.join(","));

// super() from inside an arrow rebinds the constructor's receiver.
class ViaArrow extends Swap {
    constructor() {
        const init = () => super("via-arrow");
        init();
        this.x = 2;
    }
}
const v = new ViaArrow();
console.log("via arrow:", v.tag, v.x, v.fromBase);

// The call's own value is the receiver.
class ValueOf extends Swap {
    constructor() { const r = super("value"); this.same = r === this; }
}
console.log("value:", new ValueOf().same);

// A branch around the call: both arms rebind, the join reads the right one.
class Branch extends Swap {
    constructor(a) { if (a) super("a"); else super("b"); this.k = 1; }
}
console.log("branch:", new Branch(true).tag, new Branch(false).tag, new Branch(false).k);

// Inside a try: the receiver still crosses the handler edge.
class Tried extends Swap {
    constructor() { try { super("tried"); } catch (e) {} this.t = 3; }
}
const t = new Tried();
console.log("try:", t.tag, t.t);

// Explicit returns: an object wins, anything else yields the receiver.
class RetObj extends Swap { constructor() { super("x"); return { own: true }; } }
class RetUndef extends Swap { constructor() { super("undef"); return; } }
class RetVoid extends Swap { constructor() { super("void"); return undefined; } }
console.log("returns:", new RetObj().own, new RetUndef().tag, new RetVoid().tag);

// The ordinary case is untouched: a base that returns nothing leaves the
// allocated instance as the receiver, with the derived prototype.
class Base { constructor() { this.b = 1; } }
class Derived extends Base { constructor() { super(); this.d = 2; } m() { return this.b + this.d; } }
const o = new Derived();
console.log("ordinary:", o.m(), o instanceof Derived, Object.getPrototypeOf(o) === Derived.prototype);

// Two levels: the object the innermost base returns reaches the outermost
// constructor through every hop.
class Mid extends Swap { constructor() { super("mid"); this.mid = true; } }
class Top extends Mid { constructor() { super(); this.top = true; } }
const tp = new Top();
console.log("two levels:", tp.tag, tp.mid, tp.top, tp.fromBase);

// A function base (not a class) returning an object, reached through super().
function FnBase() { return { fn: true }; }
class FromFn extends FnBase { constructor() { super(); this.e = 1; } }
const ff = new FromFn();
console.log("fn base:", ff.fn, ff.e, ff instanceof FromFn);

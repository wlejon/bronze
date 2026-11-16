// The bindings `super` reads (the active function, the home object) of a
// class the top level defines once live in the module record; every other
// class keeps its own pair, so each evaluation of a class in a loop sees its
// own base.

class A {
    who() { return 'A'; }
    static s() { return 'sA'; }
}
class B {
    who() { return 'B'; }
    static s() { return 'sB'; }
}

class D extends A {
    constructor() { super(); this.d = 1; }
    who() { return 'D>' + super.who(); }
    static s() { return 'sD>' + super.s(); }
}
const E = class extends A {
    who() { return 'E>' + super.who(); }
};
console.log(new D().who(), D.s(), new E().who());

// Moving the chains: the lookups follow.
Object.setPrototypeOf(D.prototype, B.prototype);
Object.setPrototypeOf(D, B);
console.log(new D().who(), D.s());

// Two definitions of one class body, two bases.
const made = [];
for (const base of [A, B]) {
    class L extends base {
        who() { return 'L>' + super.who(); }
    }
    made.push(L);
}
console.log(made.map((L) => new L().who()).join(' '));

// A nested derived class inside a top-level one's method.
class Outer extends A {
    make() {
        class Inner extends B {
            who() { return 'I>' + super.who(); }
        }
        return new Inner().who() + ' ' + super.who();
    }
}
console.log(new Outer().make());

// An arrow in a derived constructor reaches super() through the record.
class F extends A {
    constructor() {
        const go = () => super();
        go();
        this.f = 2;
    }
}
console.log(new F().f, new F().who());

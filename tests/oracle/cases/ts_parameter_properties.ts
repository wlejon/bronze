// TypeScript parameter properties: `constructor(public x)` assigns `this.x`
// when the constructor runs. In a derived class `this` exists only once
// super() has returned, so the assignment goes right after the super call,
// wherever it is: after statements that compute its arguments, in each arm of
// an `if`, inside a `try`, or in the middle of an expression (where the
// expression still evaluates to what super() returned).

class Base {
    constructor(public tag: string) {
        console.log("Base sees", this.tag);
    }
}

class AfterStatements extends Base {
    constructor(public n: number) {
        const t = "d" + n;
        console.log("before super");
        super(t);
        console.log("after super", this.n);
    }
}
const a = new AfterStatements(3);
console.log("AfterStatements", a.tag, a.n);

class InBranches extends Base {
    constructor(public n: number, readonly flag: boolean) {
        if (flag) {
            super("yes");
        } else super("no");
        console.log("branches", this.n, this.flag);
    }
}
new InBranches(1, true);
new InBranches(2, false);

class InExpression extends Base {
    constructor(private n: number) {
        const v = (super("expr"), this.n + 1);
        console.log("expression", v);
    }
}
new InExpression(10);

class ValueOfSuper extends Base {
    constructor(protected n: number) {
        const self = super("value");
        console.log("super() value is this", self === this, this.n);
    }
}
new ValueOfSuper(4);

class InTry extends Base {
    constructor(public n: number) {
        try {
            super("try");
        } finally {
            console.log("finally", this.n);
        }
    }
}
new InTry(5);

// No super call: the assignments come first.
class Plain {
    constructor(public a: number, private b = a * 2, readonly c?: string) {
        console.log("plain", this.a, this.b, this.c);
    }
    sum() { return this.a + this.b; }
}
const p = new Plain(3);
console.log("plain sum", p.sum(), Object.keys(p).join(","));

// A plain parameter beside property ones is not assigned.
class Mixed extends Base {
    constructor(public kept: number, dropped: number) {
        super("mixed" + dropped);
    }
}
const m = new Mixed(1, 2);
console.log("mixed", m.kept, "dropped" in m, m.tag);

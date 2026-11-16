// The heritage is evaluated once, at class definition (15.7.14 step 5), and
// super() constructs through the [[Prototype]] of the active function
// (13.3.7.1 GetSuperConstructor), not through the heritage expression read
// again, nor through the binding the source named.

let evals = 0;
function pick(k) {
    evals++;
    return k;
}

class A {
    constructor(x) {
        this.tag = 'A' + x;
    }
    who() {
        return 'A.who';
    }
    static sw() {
        return 'A.sw';
    }
}
class B {
    constructor(x) {
        this.tag = 'B' + x;
    }
    who() {
        return 'B.who';
    }
    static sw() {
        return 'B.sw';
    }
}

class D extends pick(A) {
    constructor(x) {
        super(x);
        this.d = 1;
    }
    who() {
        return 'D>' + super.who();
    }
    static sw() {
        return 'D>' + super.sw();
    }
}
console.log('evals after definition', evals);
for (let i = 0; i < 3; i++) {
    const d = new D(i);
    console.log(d.tag, d.d, d.who(), D.sw());
}
console.log('evals after constructions', evals);

// An implicit constructor forwards through the same lookup.
class E extends pick(A) {}
console.log(new E(7).tag, new E(8).tag, 'evals', evals);

// Rebinding the name the source wrote does not change the base.
let Base = A;
class F extends Base {
    constructor() {
        super('f');
    }
}
Base = B;
console.log(new F().tag, new F() instanceof A);

// Changing the constructor's [[Prototype]] after definition does.
Object.setPrototypeOf(D, B);
const d2 = new D(5);
console.log(d2.tag, d2 instanceof D, d2 instanceof A, d2.who(), D.sw());
Object.setPrototypeOf(E, B);
console.log(new E(9).tag);

// super.m starts at the home object's [[Prototype]]: moving D.prototype's
// chain moves it, and the constructor's does not.
Object.setPrototypeOf(D.prototype, B.prototype);
console.log(new D(6).who());

// From an arrow inside the constructor, and on a checked path.
class G extends pick(A) {
    constructor(flag) {
        const go = () => super('g');
        if (flag) go();
        else super('h');
    }
}
console.log(new G(true).tag, new G(false).tag, 'evals', evals);
Object.setPrototypeOf(G, B);
console.log(new G(true).tag, new G(false).tag);

// Static field initializers and static blocks see the same home object.
class H extends pick(A) {
    static s = super.sw();
    static {
        H.t = super.sw();
    }
}
console.log(H.s, H.t, 'evals', evals);

// A heritage with side effects in a class expression made in a loop runs once
// per evaluation of the class.
const made = [];
for (let i = 0; i < 3; i++) {
    const K = class extends pick(i % 2 ? A : B) {
        constructor() {
            super(i);
        }
    };
    made.push(new K().tag + new K().tag);
}
console.log(made.join(','), 'evals', evals);

// A [[Prototype]] that is no constructor makes super() throw.
class N extends A {}
Object.setPrototypeOf(N, {});
try {
    new N(1);
    console.log('no throw');
} catch (e) {
    console.log('N', e instanceof TypeError);
}

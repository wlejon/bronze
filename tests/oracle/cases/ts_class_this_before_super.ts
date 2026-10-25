// The receiver before super() in TypeScript: the same ReferenceErrors, and the
// parameter-property assignments node's transform puts after each super()
// call still run there — after a statement, in an `if` arm, and in the middle
// of an expression.

class Base {
  made: boolean;
  constructor(public tag: string = "base") {
    this.made = true;
  }
}

function attempt(label: string, make: () => object): void {
  try {
    console.log(label, "ok", JSON.stringify(make()));
  } catch (e) {
    const err = e as Error;
    console.log(label, err.constructor.name, err.message);
  }
}

class ReadsFirst extends Base {
  constructor(public x: number) {
    console.log("typed field before super:", typeof x);
    const peek = (this as Base).made;
    super();
  }
}
attempt("read", () => new ReadsFirst(1));

class AfterStatement extends Base {
  late: number;
  constructor(public x: number, readonly y: number) {
    const sum = x + y;
    super("after");
    this.late = sum;
  }
}
attempt("statement", () => new AfterStatement(1, 2));

class InBranch extends Base {
  constructor(public x: number, flag: boolean) {
    if (flag) {
      super("then");
    } else {
      super("else");
    }
  }
}
attempt("branch then", () => new InBranch(3, true));
attempt("branch else", () => new InBranch(4, false));

class InExpression extends Base {
  constructor(public x: number) {
    const self = super("expr");
    console.log("same receiver:", self === this, this.x);
  }
}
attempt("expression", () => new InExpression(5));

class Escapes extends Base {
  constructor(public x: number) {
    const f = (): number => this.x;
    console.log("arrow made");
    f();
    super();
  }
}
attempt("arrow", () => new Escapes(6));

let total = 0;
for (let i = 0; i < 500; i++) total += new AfterStatement(i, 1).late;
console.log(total);

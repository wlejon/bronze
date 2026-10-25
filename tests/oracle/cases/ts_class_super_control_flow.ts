// super() in a branch, a loop and an arrow of a typed derived constructor.
// Parameter properties are refused only where node's transform could not
// place them (a super() inside an arrow), so these classes declare plain
// typed fields where they call it from an arrow.

class Base {
  constructor(public tag: string) {}
}

function attempt(label: string, make: () => object): void {
  try {
    console.log(label, "ok", JSON.stringify(make()));
  } catch (e) {
    console.log(label, (e as Error).constructor.name, (e as Error).message);
  }
}

class Branch extends Base {
  count: number = 0;
  constructor(public x: number, which: boolean) {
    if (which) {
      super("yes");
      this.count = 1;
    } else {
      super("no");
    }
    this.count += 10;
  }
}
attempt("branch yes", () => new Branch(1, true));
attempt("branch no", () => new Branch(2, false));

class Loop extends Base {
  done: boolean;
  constructor(public x: number, times: number) {
    let i = 0;
    do {
      super("loop");
      i++;
    } while (i < times);
    this.done = true;
  }
}
attempt("loop once", () => new Loop(1, 1));
attempt("loop twice", () => new Loop(1, 2));

class Arrow extends Base {
  value: number;
  late: string = "late";
  constructor(v: number) {
    const init = (): void => {
      super("arrow");
    };
    init();
    this.value = v;
  }
}
attempt("arrow", () => new Arrow(3));

class ArrowEarly extends Base {
  value: number;
  constructor(v: number) {
    const init = (): void => {
      super("arrow");
    };
    this.value = v;
    init();
  }
}
attempt("arrow early", () => new ArrowEarly(4));

let s = 0;
for (let i = 0; i < 200; i++) s += new Branch(i, i % 2 === 0).count + new Arrow(i).value;
console.log(s);

// super() inside a branch, a loop, a switch, a try and a labeled block: the
// receiver is bound exactly on the paths that called it, the fields are
// defined when the call returns wherever it is, and a path that calls it
// twice throws on the second.

class Base {
  constructor(tag) {
    this.tag = tag;
  }
}

function attempt(label, make) {
  try {
    console.log(label, "ok", JSON.stringify(make()));
  } catch (e) {
    console.log(label, e.constructor.name, e.message);
  }
}

class Branch extends Base {
  field = "f";
  constructor(which) {
    if (which === 1) {
      super("one");
    } else if (which === 2) {
      super("two");
      this.extra = true;
    }
    this.after = which;
  }
}
attempt("branch 1", () => new Branch(1));
attempt("branch 2", () => new Branch(2));
attempt("branch 3", () => new Branch(3));

class Ternary extends Base {
  constructor(flag) {
    flag ? super("t") : super("f");
    this.done = true;
  }
}
attempt("ternary", () => new Ternary(true));
attempt("ternary else", () => new Ternary(false));

class Logical extends Base {
  constructor(flag) {
    flag && super("and");
    this.flag = flag;
  }
}
attempt("logical yes", () => new Logical(true));
attempt("logical no", () => new Logical(false));

class Loop extends Base {
  field = 0;
  constructor(times) {
    let i = 0;
    while (i < times) {
      super("loop " + i);
      i++;
    }
    this.field = this.field + 1;
  }
}
attempt("loop 0", () => new Loop(0));
attempt("loop 1", () => new Loop(1));
attempt("loop 2", () => new Loop(2));

class ForLoop extends Base {
  constructor(n) {
    for (let i = 0; i < 3; i++) {
      if (i === n) super("for " + i);
    }
    this.n = n;
  }
}
attempt("for hit", () => new ForLoop(2));
attempt("for miss", () => new ForLoop(5));

class Switch extends Base {
  constructor(k) {
    switch (k) {
      case "a":
        super("A");
        break;
      case "b":
        super("B");
      // falls through
      case "c":
        this.c = true;
        break;
      default:
        super("D");
    }
  }
}
attempt("switch a", () => new Switch("a"));
attempt("switch b", () => new Switch("b"));
attempt("switch c", () => new Switch("c"));
attempt("switch d", () => new Switch("d"));

class Try extends Base {
  constructor(fail) {
    try {
      if (fail) throw new Error("before");
      super("try");
    } catch (e) {
      super("catch");
    } finally {
      this.finally = true;
    }
  }
}
attempt("try", () => new Try(false));
attempt("catch", () => new Try(true));

class Labeled extends Base {
  constructor(skip) {
    block: {
      if (skip) break block;
      super("labeled");
    }
    this.skip = skip;
  }
}
attempt("labeled", () => new Labeled(false));
attempt("labeled skip", () => new Labeled(true));

// The checked constructors many times over, mixed with their failures.
let ok = 0;
let failed = 0;
for (let i = 0; i < 300; i++) {
  try {
    ok += new Branch(1 + (i % 3)).after;
  } catch (e) {
    failed++;
  }
}
console.log(ok, failed);

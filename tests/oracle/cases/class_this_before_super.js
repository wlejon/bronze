// In a derived constructor the receiver does not exist until super() returns
// (9.1.1.3 BindThisValue), and every way of touching it before then is a
// ReferenceError: a read, a write, passing it along, `super.x`, an arrow that
// captures it, and a parameter default that names it.

class Base {
  constructor() {
    this.made = true;
  }
  m() {
    return "base m";
  }
}

function attempt(label, make) {
  try {
    const o = make();
    console.log(label, "ok", JSON.stringify(o));
  } catch (e) {
    console.log(label, e.constructor.name, e.message);
  }
}

attempt("read", () => new (class extends Base {
  constructor() {
    const x = this.made;
    super();
  }
})());

attempt("write", () => new (class extends Base {
  constructor() {
    this.x = 1;
    super();
  }
})());

attempt("pass", () => new (class extends Base {
  constructor() {
    console.log(this);
    super();
  }
})());

attempt("bare this", () => new (class extends Base {
  constructor() {
    this;
    super();
  }
})());

attempt("super member", () => new (class extends Base {
  constructor() {
    super.m();
    super();
  }
})());

attempt("arrow", () => new (class extends Base {
  constructor() {
    const get = () => this.made;
    get();
    super();
  }
})());

attempt("arrow after", () => new (class extends Base {
  constructor() {
    const get = () => this.made;
    super();
    this.seen = get();
  }
})());

attempt("default param", () => new (class extends Base {
  constructor(a = this) {
    super();
  }
})());

attempt("in super args", () => new (class extends Base {
  constructor() {
    super(this.x);
  }
})());

attempt("after super", () => new (class extends Base {
  constructor() {
    super();
    this.after = this.made;
  }
})());

// The side effects before the throw happen; nothing after it does.
const log = [];
attempt("order", () => new (class extends Base {
  constructor() {
    log.push("before");
    log.push(this.made);
    log.push("not reached");
    super();
  }
})());
console.log(JSON.stringify(log));

// A this-access in a branch that is not taken does not throw.
class Maybe extends Base {
  constructor(early) {
    if (early) this.early = 1;
    super();
    this.late = 2;
  }
}
attempt("branch not taken", () => new Maybe(false));
attempt("branch taken", () => new Maybe(true));

// A caught ReferenceError leaves the constructor free to call super().
class Recovers extends Base {
  constructor() {
    try {
      this.x = 1;
    } catch (e) {
      log.push(e.constructor.name);
    }
    super();
    this.y = 2;
  }
}
attempt("caught", () => new Recovers());
console.log(JSON.stringify(log));

// Many constructions through the checked paths and the unchecked ones.
class Fast extends Base {
  constructor(v) {
    super();
    this.v = v;
  }
}
let sum = 0;
for (let i = 0; i < 1000; i++) sum += new Fast(i).v + new Maybe(false).late;
console.log(sum);

// super() inside an arrow function in the constructor: the arrow shares the
// constructor's receiver binding, so calling it binds `this` for the
// constructor, calling it again throws, and the fields are defined when the
// arrow's call returns.

const log = [];
class Base {
  constructor(tag) {
    this.tag = tag;
    log.push("base " + tag);
  }
}

function attempt(label, make) {
  try {
    console.log(label, "ok", JSON.stringify(make()));
  } catch (e) {
    console.log(label, e.constructor.name, e.message);
  }
}

class Arrow extends Base {
  field = "f";
  constructor() {
    const init = () => super("arrow");
    init();
    this.after = this.field;
  }
}
attempt("arrow", () => new Arrow());

class ArrowValue extends Base {
  constructor() {
    const init = () => super("value");
    const self = init();
    this.same = self === this;
  }
}
attempt("value", () => new ArrowValue());

class ArrowTwice extends Base {
  constructor() {
    const init = () => super("twice");
    init();
    try {
      init();
    } catch (e) {
      this.second = e.message;
    }
  }
}
attempt("twice", () => new ArrowTwice());

class ArrowNever extends Base {
  constructor() {
    const init = () => super("never");
  }
}
attempt("never", () => new ArrowNever());

class ArrowThenThis extends Base {
  constructor() {
    const init = () => {
      super("then this");
      this.inner = true;
      return this;
    };
    init();
    this.outer = true;
  }
}
attempt("then this", () => new ArrowThenThis());

class ThisBeforeArrow extends Base {
  constructor() {
    const init = () => super("late");
    this.early = 1;
    init();
  }
}
attempt("this before", () => new ThisBeforeArrow());

// A nested arrow, and an arrow passed to a function that calls it.
class Nested extends Base {
  constructor() {
    const outer = () => {
      const inner = () => super("nested");
      inner();
    };
    outer();
    this.n = 1;
  }
}
attempt("nested", () => new Nested());

function call(f) {
  return f();
}
class Passed extends Base {
  constructor() {
    call(() => super("passed"));
    this.p = 1;
  }
}
attempt("passed", () => new Passed());
console.log(log.join(" | "));

// The base constructed from an arrow sees the constructor's new.target, and
// so does the arrow itself, however it is called.
class Seen {
  constructor() {
    this.seen = new.target && new.target.name;
  }
}
class SeenArrow extends Seen {
  constructor() {
    const read = () => new.target && new.target.name;
    call(() => super());
    this.arrow = call(read);
  }
}
attempt("new.target", () => new SeenArrow());

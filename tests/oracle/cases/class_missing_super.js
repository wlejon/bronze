// A derived constructor that returns without having called super() is a
// ReferenceError, unless what it returns is an object; a primitive return
// value is a TypeError whether or not super() ran.

class Base {
  constructor() {
    this.base = 1;
  }
}

function attempt(label, make) {
  try {
    console.log(label, "ok", JSON.stringify(make()));
  } catch (e) {
    console.log(label, e.constructor.name, e.message);
  }
}

attempt("empty body", () => new (class extends Base {
  constructor() {}
})());

attempt("bare return", () => new (class extends Base {
  constructor() {
    return;
  }
})());

attempt("return undefined", () => new (class extends Base {
  constructor() {
    return undefined;
  }
})());

attempt("return object", () => new (class extends Base {
  constructor() {
    return { replaced: true };
  }
})());

attempt("return function", () => new (class extends Base {
  constructor() {
    return function f() {};
  }
})());

attempt("return primitive", () => new (class extends Base {
  constructor() {
    return 42;
  }
})());

attempt("return null", () => new (class extends Base {
  constructor() {
    return null;
  }
})());

attempt("return primitive after super", () => new (class extends Base {
  constructor() {
    super();
    return "str";
  }
})());

attempt("return undefined after super", () => new (class extends Base {
  constructor() {
    super();
    return undefined;
  }
})());

// Only on the path that skips super().
class Sometimes extends Base {
  constructor(call) {
    if (call) super();
  }
}
attempt("sometimes yes", () => new Sometimes(true));
attempt("sometimes no", () => new Sometimes(false));

class EarlyReturn extends Base {
  constructor(early) {
    if (early) return;
    super();
    this.full = true;
  }
}
attempt("early return", () => new EarlyReturn(true));
attempt("no early return", () => new EarlyReturn(false));

// A constructor that throws before super() throws its own error.
attempt("throws first", () => new (class extends Base {
  constructor() {
    throw new Error("own");
  }
})());

// The implicit constructor forwards to the base, and so is always fine.
class Implicit extends Base {}
attempt("implicit", () => new Implicit());

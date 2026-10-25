// A second super() is a ReferenceError — but only after the base has been
// constructed again (13.3.7.1: Construct, then BindThisValue throws). The
// second construction makes a NEW object; the instance the first call bound is
// not handed to the base a second time.

const log = [];
class Base {
  constructor(n) {
    this.count = (this.count || 0) + 1;
    this.n = n;
    log.push("base " + n);
  }
}

class Twice extends Base {
  constructor() {
    super(1);
    try {
      super(2);
    } catch (e) {
      log.push(e.constructor.name + ": " + e.message);
    }
    log.push("after: n=" + this.n + " count=" + this.count);
  }
}
const t = new Twice();
console.log(JSON.stringify(t));
console.log(log.splice(0).join("\n"));

// Uncaught, the construction fails.
class Uncaught extends Base {
  constructor() {
    super(1);
    super(2);
  }
}
try {
  new Uncaught();
} catch (e) {
  console.log(e.constructor.name, e.message);
}
console.log(log.splice(0).join(" | "));

// The arguments of the second call are evaluated before it throws.
class Args extends Base {
  constructor() {
    super(1);
    try {
      super((log.push("args evaluated"), 3));
    } catch (e) {
      log.push("caught");
    }
  }
}
new Args();
console.log(log.splice(0).join(" | "));

// A base that returns a different object: the first call binds that object,
// and the second still throws.
class Other {
  constructor() {
    return { other: true };
  }
}
class Rebound extends Other {
  constructor() {
    super();
    this.mine = 1;
    try {
      super();
    } catch (e) {
      this.second = e.constructor.name;
    }
  }
}
console.log(JSON.stringify(new Rebound()));

// The second call reaching the base through new.target: the fresh object is
// made from the derived class, so the base sees the derived prototype.
class Sees {
  constructor() {
    log.push(Object.getPrototypeOf(this) === Seen.prototype ? "derived proto" : "other proto");
  }
}
class Seen extends Sees {
  constructor() {
    super();
    try {
      super();
    } catch (e) {}
  }
}
new Seen();
console.log(log.splice(0).join(" | "));

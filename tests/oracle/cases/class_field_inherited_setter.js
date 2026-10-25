// A field is DEFINED on the instance (CreateDataPropertyOrThrow), so an
// accessor of the same name on the prototype chain never sees it: the setter
// does not run, and the own data property shadows the getter from then on.

const log = [];
class Base {
  set a(v) {
    log.push("set a " + v);
  }
  get a() {
    return "getter";
  }
  set b(v) {
    log.push("set b " + v);
  }
}

class Declared extends Base {
  a;
  constructor() {
    super();
    log.push("ctor sees " + this.a);
    this.a = 1;
  }
}
const d = new Declared();
console.log(JSON.stringify(d), d.a, Object.getOwnPropertyNames(d).join(","));
console.log(log.splice(0).join(" | "));

// With an initializer, the same.
class Initialized extends Base {
  a = 5;
}
const i = new Initialized();
console.log(JSON.stringify(i), i.a);
console.log(JSON.stringify(log.splice(0)));

// Without the field, the constructor's assignment is a [[Set]], and it is the
// setter that runs: no own property is made.
class Undeclared extends Base {
  constructor() {
    super();
    this.a = 1;
    this.b = 2;
  }
}
const u = new Undeclared();
console.log(JSON.stringify(u), u.a);
console.log(log.splice(0).join(" | "));

// An accessor two classes up is shadowed as well, and one in the class's own
// body (on its prototype) too.
class Middle extends Base {}
class Deep extends Middle {
  b = 7;
}
console.log(JSON.stringify(new Deep()), JSON.stringify(log.splice(0)));

class Own {
  set c(v) {
    log.push("own set c " + v);
  }
  c = 3;
}
const o = new Own();
console.log(JSON.stringify(o), Object.getOwnPropertyDescriptor(o, "c").writable);
console.log(JSON.stringify(log.splice(0)));

// A read-only data property on the prototype does not stop a definition.
class Frozen {}
Object.defineProperty(Frozen.prototype, "f", { value: "proto", writable: false });
class Thaw extends Frozen {
  f = "own";
}
console.log(new Thaw().f);

// A base from an expression, whose chain no static reading can see.
function mixin(B) {
  return class extends B {
    set m(v) {
      log.push("mixin set m " + v);
    }
  };
}
class Mixed extends mixin(Object) {
  m = 1;
}
console.log(JSON.stringify(new Mixed()), JSON.stringify(log.splice(0)));

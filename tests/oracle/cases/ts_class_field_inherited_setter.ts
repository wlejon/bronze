// A typed field declaration defines its property, so an inherited accessor of
// the name never sees the constructor's later assignment: the setter does not
// run and the instance owns the key. Leaving the declaration out is what makes
// the assignment reach the setter.

const log: string[] = [];
class Base {
  set a(v: number) {
    log.push("set a " + v);
  }
  get a(): number | string {
    return "getter";
  }
}

class Declared extends Base {
  a: number;
  constructor() {
    super();
    this.a = 1;
  }
}
const d = new Declared();
console.log(JSON.stringify(d), d.a);
console.log(JSON.stringify(log.splice(0)));

class Undeclared extends Base {
  constructor() {
    super();
    this.a = 2;
  }
}
const u = new Undeclared();
console.log(JSON.stringify(u), u.a);
console.log(JSON.stringify(log.splice(0)));

// A parameter property is a declared field too.
class Param extends Base {
  constructor(public a: number) {
    super();
  }
}
const p = new Param(3);
console.log(JSON.stringify(p), p.a);
console.log(JSON.stringify(log.splice(0)));

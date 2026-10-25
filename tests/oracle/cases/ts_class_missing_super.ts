// A derived TypeScript constructor that returns before super(): a
// ReferenceError, an object return excepted, with fields and parameter
// properties declared — none of them ever defined.

class Base {
  constructor() {}
}

function attempt(label: string, make: () => object): void {
  try {
    console.log(label, "ok", JSON.stringify(make()));
  } catch (e) {
    console.log(label, (e as Error).constructor.name, (e as Error).message);
  }
}

class NoSuper extends Base {
  field: number = 1;
  constructor() {
    // @ts-ignore: the missing call is the point
  }
}
attempt("no super", () => new NoSuper());

class Guarded extends Base {
  field: number = 2;
  constructor(public x: number, skip: boolean) {
    if (skip) return;
    super();
  }
}
attempt("guarded ok", () => new Guarded(1, false));
attempt("guarded skip", () => new Guarded(1, true));

class Replaces extends Base {
  constructor() {
    return { replaced: "yes" } as Replaces;
  }
}
attempt("replaces", () => new Replaces());

// ECMA-262 10.2.11 / 15.7: ALL parts of a class definition are strict mode
// code, whether or not anything said so. The file around them is sloppy —
// there is no directive anywhere in it — so every TypeError below comes from
// the class body's own strictness and from nothing else.
//
// This is the rule that cannot be reached by a Directive Prologue, which is
// why it needs a case of its own: the prologue mechanism and the class rule
// are two independent sources of the same flag, and a parser that implemented
// only the first would pass `cases/strict_mode` and fail here.
//
// `target` has a getter and no setter, so a write to it is 10.1.9.2 step 5.c
// returning false — the same refusal `cases/strict_mode` uses, chosen here
// because it needs no `Object.defineProperty` to set up and so keeps the case
// about strictness alone.
//
// What each line pins:
//
// 1. An instance method is strict.
// 2. A `static` method is strict, which is a separate path in bronze: a static
//    member is an own property of the constructor function rather than of the
//    prototype, and it is defined by a different helper.
// 3. A class ACCESSOR body is strict. Its write throws, so the `return` below
//    it never runs and the getter's value is never printed.
// 4. The implicitly-synthesized constructor of a class that writes none is
//    class code too — reached here through a subclass, whose default
//    constructor forwards to `super`. The write is in `Base`'s constructor,
//    which is written out; what this line pins is that the derived class's
//    invisible one carries the mode across.
// 5. Outside the class body the file is sloppy again, so the same write on the
//    same object is the silent no-op it always was, and the getter still says
//    1. The class body raised the flag and gave it back.
const target = {
  get g() {
    return 1;
  }
};

class Writer {
  constructor(t) {
    this.t = t;
  }
  write() {
    this.t.g = 2;
  }
  static writeStatic(t) {
    t.g = 3;
  }
  get failing() {
    this.t.g = 4;
    return "unreached";
  }
}

class Base {
  constructor(t) {
    t.g = 5;
  }
}
class Derived extends Base {}

const w = new Writer(target);
try {
  w.write();
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError, e.name);
}
try {
  Writer.writeStatic(target);
  console.log("no throw");
} catch (e) {
  console.log(e.name);
}
try {
  console.log(w.failing);
} catch (e) {
  console.log(e.name);
}
try {
  new Derived(target);
  console.log("no throw");
} catch (e) {
  console.log(e.name);
}

target.g = 6;
console.log(target.g);

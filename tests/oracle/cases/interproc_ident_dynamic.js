// The one construct that takes interprocedural identity away from the whole
// program: a call whose property name is decided at run time.
//
// `o[k](arg)` with a `k` no fixed set of literals covers can reach any method in
// the program, so every method gives up its parameters — and the answers must
// not move by a character. Kept in its own case because the poison is
// module-wide: a file containing one of these proves nothing about the sites the
// mechanism does claim, and interproc_ident.js is where those live.

class A {
  hit(p) {
    return p.v + 1;
  }
}
class B {
  hit(p) {
    return p.v + 2;
  }
}
class V {
  constructor(v) {
    this.v = v;
  }
}

function callByName(o, k, arg) {
  return o[k](arg);
}

const a = new A();
const b = new B();
console.log(a.hit(new V(1)) + "," + b.hit(new V(1)));
console.log(callByName(a, "hit", new V(10)) + "," + callByName(b, "hit", new V(10)));

// A computed call whose name IS pinnable stands beside it, so the case also
// shows the two paths meeting: the module is already fully poisoned, and this
// still answers the same.
const named = "hit";
console.log(a[named](new V(100)) + "," + b[named](new V(100)));

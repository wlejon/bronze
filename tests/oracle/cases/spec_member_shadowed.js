// A method and a getter found on the prototype chain, used from one site for
// receivers of two classes. Partway an instance gets an own method, then a
// method and a getter added to the intermediate prototype shadow the
// holder's, so a harvest of the run names the shadowing pair: the holder's
// members and the own method must take the generic path, the shadowing ones
// the speculated path.
class A {
  f() { return 1; }
  get g() { return this.v * 2; }
}
class B extends A { constructor(v) { super(); this.v = v; } }
class C extends B { constructor(v) { super(v); this.w = 0; } }
function run(objs, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const o = objs[i % objs.length];
    s += o.f() + o.g;
  }
  return s;
}
function main() {
  const objs = [new B(1), new C(2), new B(3), new C(4)];
  const out = [];
  out.push(run(objs, 20000));
  const odd = new B(5);
  odd.f = function () { return 7; };
  out.push(run([odd, objs[1]], 20000));
  B.prototype.f = function () { return 2; };
  Object.defineProperty(B.prototype, "g", { get() { return this.v * 3; }, configurable: true });
  out.push(run(objs, 20000));
  out.push(run([new C(6), new B(7)], 20000));
  console.log(out.join(" "));
}
main();

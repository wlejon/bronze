// A polymorphic method site over three classes, then a fourth (a subclass
// overriding the method) and a fifth (one inheriting it) arrive late.
function main() {
  class A { constructor(v) { this.v = v; } f() { return this.v; } }
  class B { constructor(v) { this.v = v; this.b = 1; } f() { return this.v * 2; } }
  class C { constructor(v) { this.c = 0; this.v = v; } f() { return this.v * 3; } }
  class D extends A { f() { return -this.v; } }
  class E extends B { }
  const pool = [new A(1), new B(2), new C(3)];
  let s = 0;
  for (let i = 0; i < 45000; i++) {
    if (i === 20000) pool.push(new D(4));
    if (i === 30000) pool.push(new E(5));
    s += pool[i % pool.length].f();
  }
  console.log(s, pool.map(o => o.f()).join(","));
}
main();

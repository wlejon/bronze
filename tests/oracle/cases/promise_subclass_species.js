// Every place 27.2 builds a promise through a CONSTRUCTOR rather than through
// %Promise%, and the one hook that opts back out.
//
// 27.2.5.4 `then` calls SpeciesConstructor(promise, %Promise%) (7.3.20) and
// hands the result to NewPromiseCapability (27.2.1.5), so the promise it
// returns is built by the receiver's constructor's @@species — the subclass,
// for an ordinary one. `catch` and `finally` are defined in terms of `then`
// (27.2.5.1, 27.2.5.3) and inherit that for free. The statics take the other
// route: `Promise.resolve`, `reject`, `all`, `allSettled`, `any` and `race` are
// specified over `this`, so calling them on the subclass constructs through it.
//
// The first block asks each of those separately, synchronously, because the
// interesting fact is WHICH CONSTRUCTOR ran and not when the reaction did.
//
// `static get [Symbol.species]() { return Promise; }` is the opt-out, and it is
// the reason SpeciesConstructor exists rather than a plain `constructor` read:
// `Opted.resolve(1)` is still an `Opted` — that is `this`, not species — while
// `.then` on it is a plain promise.
//
// The tail is one strictly sequential chain, so its order is the resolution
// order and nothing about subclassing. `finally` passes its value through
// (27.2.5.3 step 6's thunk returns the original), which is why "d" reads 10 and
// not undefined.

class MyP extends Promise {}

const p = new MyP((res) => res(1));
console.log("ctor", p instanceof MyP, p instanceof Promise);
console.log("then", p.then((v) => v) instanceof MyP);
console.log("catch", p.catch(() => 0) instanceof MyP);
console.log("finally", p.finally(() => {}) instanceof MyP);
console.log("resolve", MyP.resolve(1) instanceof MyP);
console.log("reject", MyP.reject(1).catch(() => {}) instanceof MyP);
console.log("all", MyP.all([]) instanceof MyP);
console.log("allSettled", MyP.allSettled([]) instanceof MyP);
console.log("race", MyP.race([MyP.resolve(1)]) instanceof MyP);
console.log("any", MyP.any([MyP.resolve(1)]) instanceof MyP);

class Opted extends Promise {
  static get [Symbol.species]() {
    return Promise;
  }
}
const o = new Opted((res) => res(1));
console.log("opt", o instanceof Opted, o.then((v) => v) instanceof Opted, Opted.resolve(2) instanceof Opted);

// The intrinsic is untouched by any of the above.
const q = Promise.resolve(1);
console.log("plain", q instanceof MyP, q instanceof Promise, q.then((v) => v) instanceof Promise);

MyP.resolve(2)
  .then((v) => {
    console.log("a", v);
    return v + 1;
  })
  .then((v) => {
    throw new Error("e" + v);
  })
  .catch((e) => {
    console.log("b", e.message);
    return 10;
  })
  .finally(() => console.log("c"))
  .then((v) => console.log("d", v))
  .then(() => MyP.all([MyP.resolve(1), 2, MyP.resolve(3)]))
  .then((xs) => console.log("all", xs.join(","), Array.isArray(xs)))
  .then(() => MyP.allSettled([MyP.resolve(1), MyP.reject(new Error("x"))]))
  .then((rs) => console.log("allSettled", rs.map((r) => r.status).join(",")))
  .then(() => MyP.any([MyP.reject(new Error("a")), MyP.resolve("b")]))
  .then((v) => console.log("any", v))
  .then(() => MyP.race([MyP.resolve("r"), new MyP(() => {})]))
  .then((v) => console.log("race", v));

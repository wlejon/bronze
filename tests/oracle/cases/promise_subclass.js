// A Promise subclass, which the language supports through two hooks that have
// to agree. `MyPromise.resolve` (27.2.4.7) constructs through `this` rather
// than through %Promise%, so the result is a MyPromise; and `then`
// (27.2.5.4 -> 27.2.5.4.1 NewPromiseCapability with SpeciesConstructor) builds
// its result through the receiver's constructor's @@species, which for an
// ordinary subclass is the subclass — so the chain stays in the subclass all
// the way down.
//
// The observable order below is the ordinary microtask one and is not about
// subclassing: both `console.log` lines run synchronously, then the two
// reactions registered on `p` run in registration order, and the reaction on
// `q` runs after the one that resolves it.
//
// What this pins is the pair of hooks meeting: `new` on a constructor whose
// `extends` chain reaches `Promise` allocates the slotted promise object and
// binds it as the derived `this` (runtime/native_base.h), and `then` builds its
// result through NewPromiseCapability(SpeciesConstructor(p, %Promise%)) rather
// than through the intrinsic. Either one alone would answer `false` on the
// second line.

class MyPromise extends Promise {}

const p = MyPromise.resolve(1);
console.log(p instanceof MyPromise, p instanceof Promise);

p.then((v) => {
  console.log("then", v);
});
const q = p.then((v) => v + 1);
console.log(q instanceof MyPromise, q instanceof Promise);

q.then((v) => {
  console.log("q", v);
});

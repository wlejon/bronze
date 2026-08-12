// `a?.b`, `a?.[k]` and `a?.()`, and exactly how far a short circuit reaches.
//
// Derived from ECMA-262 13.3.9 (Optional Chains) and 13.3.1 (the Member
// Access grammar):
//
// 1. An OptionalExpression evaluates its base once. If the base is undefined or
// null, the ENTIRE OptionalChain to its right is skipped and the result is
// undefined — the chain is one syntactic unit, not a sequence of independent
// tests. So `nn?.b.c.d` is undefined; the `.c` and `.d` links never run and
// never see the undefined that `nn?.b` would have produced. 2. Skipping is
// skipping, not evaluating-and-discarding: the arguments of a skipped call and
// the key expression of a skipped `?.[k]` are never evaluated. `evals` below is
// the witness. 3. Short-circuiting is on NULLISH, not on falsy. `""` is not
// nullish, so `empty?.length` reads and gives 0, where `nn?.length` gives
// undefined. The two answers differ, which is what makes the distinction
// observable. (`0?.x` is likewise a real read, but Number.prototype has no `x`,
// so it also lands on undefined and pins nothing extra.) 4. Parentheses end a
// chain: `(o?.a).b` is a NEW MemberExpression whose base happens to be a
// parenthesized optional expression, so `.b` is an ordinary access that does
// not inherit the short circuit. With a non-nullish `o` that is invisible,
// which is what this case pins. With a nullish one the spec says `.b` throws a
// TypeError — bronze diagnoses it as a hard runtime error, and it stays
// unpinned here because there is no `try` yet to catch it. 5. `a.b?.()` is
// still a method call: 13.3.9.1 evaluates the base as a Reference and passes
// its base value as the `this` argument, so the optional link does not silently
// turn a method into a bare function.
const obj = { a: { b: { c: 7 } }, n: null, f: function () { return "called"; } };
console.log(obj?.a?.b?.c);
console.log(obj.n?.b);
console.log(obj.missing?.b);
console.log(obj.n?.b.c.d);
console.log(obj.missing?.b.c.d);

const arr = [10, 20, 30];
console.log(arr?.[1]);
console.log(arr?.length);
const nn = null;
console.log(nn?.[0]);
console.log(nn?.length);

const empty = "";
console.log(empty?.length);
console.log(empty?.charAt(0));

let evals = 0;
function k() {
  evals = evals + 1;
  return "x";
}
const has = { x: 42 };
console.log(has?.[k()]);
console.log(evals);
console.log(nn?.[k()]);
console.log(evals);
console.log(nn?.f(k()));
console.log(evals);

console.log(obj.f?.());
console.log(obj.g?.());

const counter = {
  n: 5,
  get: function () {
    return this.n;
  },
};
console.log(counter.get?.());
console.log(counter?.get());
console.log(counter?.["n"]);

const o = { a: { b: 1 } };
console.log((o?.a).b);

// A `?.` inside an argument starts its OWN chain: short-circuiting the inner
// one must not skip the outer call. `b(undefined)` still runs.
const outer = {
  b: function (v) {
    return "b(" + v + ")";
  },
};
const innerVal = { d: "D" };
console.log(outer?.b(innerVal?.d));
console.log(outer?.b(nn?.d));

// The chain's base is evaluated exactly once even when the chain continues.
let log = "";
function side(t) {
  log = log + t;
  return { v: t };
}
console.log(side("A")?.v);
console.log(log);

function reach(x) {
  return x?.p?.q;
}
console.log(reach(null));
console.log(reach(undefined));
console.log(reach({ p: null }));
console.log(reach({ p: { q: 9 } }));

// A chain that short-circuits still has to join with the branch that did not:
// a binding assigned inside the chain's continuation must agree at the merge.
function pick(src) {
  let tag = "none";
  const v = src?.list[0];
  if (v !== undefined) tag = "got:" + v;
  return tag;
}
console.log(pick(null));
console.log(pick({ list: [11, 22] }));

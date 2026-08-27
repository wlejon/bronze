#include <cstdlib>
#include <doctest/doctest.h>

#include "types_test_helper.h"

using namespace bronze;
using namespace bronze::test;

// ---- constructor parameter identity -----------------------------------------

TEST_CASE("a constructor's parameters are joined over its construction sites") {
    const auto inferred = infer(
        "class Vec { constructor(x = 0, y = 0) { this.x = x; this.y = y; } }\n"
        "function dot(a, b) { return a.x * b.x + a.y * b.y; }\n"
        "const p = new Vec(1, 2);\n"
        "const q = new Vec();\n"
        "console.log(dot(p, q));\n");
    const auto& r = *inferred.result;
    CHECK(r.ctorParams.ctors == 1);
    CHECK(r.ctorParams.paramsNumber == 2);
    CHECK(r.ctorParams.paramsDynamic == 0);
    CHECK(r.fieldAudit.namesClean == 2);
    CHECK(r.provenFieldReads.size() > 0);
}

TEST_CASE("a default is one of a constructor's call sites") {
    const auto inferred = infer(
        "class Tag { constructor(n = 0, label = 'none') { this.n = n; this.lbl = label; } }\n"
        "function read(t) { return t.n; }\n"
        "console.log(read(new Tag()) + read(new Tag(2, 'two')));\n");
    const auto& r = *inferred.result;
    CHECK(r.ctorParams.paramsNumber == 1);
    CHECK(r.ctorParams.paramsOther == 1);
    CHECK(r.fieldAudit.namesClean == 1);
}

TEST_CASE("one non-number construction site takes the whole name back") {
    const auto inferred = infer(
        "class Box { constructor(v) { this.v = v; } }\n"
        "function read(b) { return b.v; }\n"
        "console.log(read(new Box(1)) + read(new Box('hi')));\n");
    const auto& r = *inferred.result;
    CHECK(r.ctorParams.paramsDynamic == 1);
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.provenFieldReads.empty());
}

TEST_CASE("a class read as a value gives up its parameters, and so do its bases") {
    const auto inferred = infer(
        "class Base { constructor(a = 0) { this.a = a; } }\n"
        "class Sub extends Base { constructor(a, b = 0) { super(a); this.b = b; } }\n"
        "class Free { constructor(f = 0) { this.f = f; } }\n"
        "function build(C) { return new C(1); }\n"
        "console.log(build(Sub).a + new Free(2).f);\n");
    const auto& r = *inferred.result;
    CHECK(r.ctorParams.poisons.count("the class binding is read as a value") == 1);
    CHECK(r.ctorParams.poisons.at("the class binding is read as a value") == 2);
    CHECK(r.ctorParams.ctorsSpeaking == 1);
}

TEST_CASE("super and the implicit forwarder carry a parameter into a base field") {
    const auto inferred = infer(
        "class Base { constructor(a = 0) { this.a = a; } }\n"
        "class Mid extends Base {}\n"
        "class Leaf extends Mid { constructor(v) { super(v * 2); } }\n"
        "console.log(new Mid(1).a + new Leaf(3).a);\n");
    const auto& r = *inferred.result;
    CHECK(r.ctorParams.forwarders == 1);
    CHECK(r.ctorParams.poisons.empty());
    CHECK(r.ctorParams.paramsNumber == 2);
    CHECK(r.fieldAudit.namesClean == 1);
}

TEST_CASE("a computed `new` costs only the classes its table was built from") {
    const auto inferred = infer(
        "class Arc { constructor(r = 1) { this.r = r; } }\n"
        "class Solo { constructor(s = 3) { this.s = s; } }\n"
        "const Registry = { Arc: Arc };\n"
        "function make(name) { return new Registry[name](); }\n"
        "console.log(make('Arc').r + new Solo(2).s);\n");
    const auto& r = *inferred.result;
    CHECK(r.ctorParams.ctorsSpeaking == 1);
    CHECK(r.ctorParams.unnamedNewIgnored == 1);
    CHECK(r.ctorParams.unnamedNewAll == 0);
    CHECK(r.fieldAudit.namesClean >= 1);
}

// ---- method parameter typing (Job 1) ----------------------------------------

TEST_CASE("method parameters carry Number types and feed field harvest") {
    const auto inferred = infer(
        "class Vector3 {\n"
        "  constructor() { this.x = 0; this.y = 0; this.z = 0; }\n"
        "  set(x, y, z) { this.x = x; this.y = y; this.z = z; return this; }\n"
        "}\n"
        "const v = new Vector3();\n"
        "v.set(1.0, 2.0, 3.0);\n"
        "console.log(v.x + v.y + v.z);\n");
    const auto& r = *inferred.result;
    CHECK(r.methodParams.methods >= 1);
    CHECK(r.methodParams.methodsSpeaking >= 1);
    CHECK(r.methodParams.paramsNumber >= 3);
    CHECK(r.fieldAudit.namesClean == 3);
    CHECK(r.provenFieldReads.size() > 0);
}

TEST_CASE("method parameter defaults participate in call-site joins") {
    const auto inferred = infer(
        "class Counter {\n"
        "  constructor() { this.val = 0; }\n"
        "  add(step = 1) { this.val += step; return this.val; }\n"
        "}\n"
        "const c = new Counter();\n"
        "c.add();\n"
        "c.add(5);\n"
        "console.log(c.val);\n");
    const auto& r = *inferred.result;
    CHECK(r.methodParams.methodsSpeaking >= 1);
    CHECK(r.methodParams.paramsNumber >= 1);
    CHECK(r.fieldAudit.namesClean == 1);
}

TEST_CASE("method read as a value poisons the method") {
    const auto inferred = infer(
        "class V {\n"
        "  constructor() { this.x = 0; }\n"
        "  set(x) { this.x = x; }\n"
        "}\n"
        "const v = new V();\n"
        "const fn = v.set;\n"
        "v.set(1);\n"
        "console.log(v.x);\n");
    const auto& r = *inferred.result;
    CHECK(r.methodParams.poisons.count("read as a value rather than called") >= 1);
}

TEST_CASE("unbounded receiver calling method contributes args and counts") {
    const auto inferred = infer(
        "class V {\n"
        "  constructor() { this.x = 0; }\n"
        "  set(x) { this.x = x; }\n"
        "}\n"
        "function callDyn(o) { o.set(1); }\n"
        "function escape(fn) { fn({}); }\n"
        "escape(callDyn);\n"
        "const v = new V();\n"
        "v.set(2);\n"
        "console.log(v.x);\n");
    const auto& r = *inferred.result;
    CHECK(r.methodParams.unboundedCalls >= 1);
    CHECK(r.methodParams.paramsNumber >= 1);
}

TEST_CASE("prototype mutation refutes class subtree") {
    const auto inferred = infer(
        "class Base {\n"
        "  m(x) { this.x = x; }\n"
        "}\n"
        "class Sub extends Base {\n"
        "  m(x) { super.m(x); }\n"
        "}\n"
        "Base.prototype.m = function(x) {};\n"
        "const s = new Sub();\n"
        "s.m(1);\n");
    const auto& r = *inferred.result;
    CHECK(r.methodParams.poisons.count("the method on the prototype is mutated/overwritten") >= 1);
}

TEST_CASE("reading field on `this` inside method is proven when clean") {
    const auto inferred = infer(
        "class V {\n"
        "  constructor() { this.x = 0; }\n"
        "  read() { let s = this.x; for (let i = 0; i < 2; i++) s = this.x; return s; }\n"
        "}\n"
        "console.log(new V().read());\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.fieldAudit.refusedNotBuiltHere == 0);
    CHECK(r.provenFieldReads.size() >= 2);
}

TEST_CASE("a module-scope temporary is a known receiver on the FIRST round") {
    // The three.js idiom: a scratch instance declared at module scope, reached
    // from inside a method, and a method name more than one class declares.
    //
    // `moduleBindings` is filled at the END of the top level's walk and every
    // round walks the bodies first, so before the priming rounds the receiver
    // `_ta` answered `Dynamic` on round one. A call whose receiver has no class
    // contributes its argument to EVERY method of that name — so `A` reached
    // `B.copy`'s parameter, the two joined to an object with no class at all,
    // and because the signature fold only WIDENS, the later rounds that DO
    // resolve `_ta` could never take it back. `o.x` in `B.copy` was then a
    // dynamic read of a field the whole program only ever writes numbers to.
    //
    // The argument has to be `this` rather than a parameter: a parameter is
    // still `Never` on round one, so it poisons nothing and the window closes
    // before it has a type. three.js writes `_m1.copy( this )`, which is the
    // shape that lands inside the window.
    const auto inferred = infer(
        "class A {\n"
        "  constructor() { this.x = 1; }\n"
        "  copy(o) { this.x = o.x; return this; }\n"
        "  scratch() { _ta.copy(this); return _ta.x; }\n"
        "}\n"
        "class B {\n"
        "  constructor() { this.x = 2; }\n"
        "  copy(o) { this.x = o.x; return this; }\n"
        "}\n"
        "const _ta = new A();\n"
        "const a = new A();\n"
        "const b = new B();\n"
        "const b2 = new B();\n"
        "b.copy(b2);\n"
        "console.log(a.scratch() + b.x);\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    // Every read of `.x` in the program, and the count is exact because the
    // round-one collapse costs exactly one of them: `o.x` in `B.copy`. `A.copy`
    // survives it — the argument the by-name contribution carried was an `A`,
    // and joining `A` with `A` loses nothing. Which is why the reproduction
    // needs TWO classes declaring the name and only one of them reached
    // through the module-scope temporary.
    CHECK(r.provenFieldReads.size() == 8);
}

TEST_CASE("an optimistic field audit does not decide a parameter on round one") {
    // Two lattices move through the fixpoint in opposite directions. Signatures
    // WIDEN from `Never`; field cleanliness NARROWS from "every written name
    // holds only numbers", as the rounds find the writes that refute it. Both
    // are monotone, which is all the loop needs to terminate — but the two do
    // not mix. A read taken while its name is still presumed clean answers
    // `Number`, the fold joins that in, and `join(Number, Object)` is an object
    // with no class that no later round can take back.
    //
    // `Holder.p` is defined through a descriptor, which the audit has to reason
    // its way to rather than see, so it stays presumed clean for round after
    // round while `update` hands `Mat.compose` a Number. `fromScratch` is the
    // site that would otherwise settle the question — `_scratch` is a
    // module-scope `Vec` and says so from the first round — and the join of the
    // two is what is lost.
    //
    // Under a manifest, because pin optimism is what leaves the Number alone to
    // do the damage: a Dynamic argument is set aside instead of joined, so the
    // spurious Number is the only contribution that can still poison the join.
    // three.js is this shape at scale — `Object3D` defines `position`,
    // `quaternion` and `scale` with `Object.defineProperties`, so
    // `Matrix4.compose` observed `Vector3, Quaternion, Vector3` on every round
    // but the first and still shipped with three dynamic parameters.
    const auto inferred = inferPinned(
        "class Vec {\n"
        "  constructor() { this.x = 1.5; }\n"
        "}\n"
        "class Mat {\n"
        "  constructor() { this.e = 0; }\n"
        "  compose(v) { this.e = v.x; return this; }\n"
        "  fromScratch() { return this.compose(_scratch); }\n"
        "}\n"
        "class Holder {\n"
        "  constructor() {\n"
        "    this.m = new Mat();\n"
        "    Object.defineProperty(this, 'p', { value: new Vec() });\n"
        "  }\n"
        "  update() { this.m.compose(this.p); return this.m.e; }\n"
        "}\n"
        "const _scratch = new Vec();\n"
        "const h = new Holder();\n"
        "console.log(h.update() + new Mat().fromScratch().e);\n",
        "Vec.x: number\n");
    const auto& r = *inferred.result;
    // The one parameter in the program, and it keeps its class. Without the
    // priming rounds it is `dynamic` — the exact counts, because a signature
    // that merely widened would leave `paramsObject` at zero either way.
    CHECK(r.methodParams.paramsObject == 1);
    CHECK(r.methodParams.paramsDynamic == 0);
}

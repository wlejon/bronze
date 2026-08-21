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

#include <doctest/doctest.h>

#include "types_test_helper.h"

using namespace bronze;
using namespace bronze::test;

// ---- the field-type write audit ---------------------------------------------

TEST_CASE("a field written only with numbers licenses a raw read") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.fieldAudit.refusals.empty());
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.provenFieldReads.size() >= 3);
    CHECK(r.fieldAudit.numberFieldReads == 4);
    CHECK(r.fieldAudit.refusedByAudit == 0);
}

TEST_CASE("one string write anywhere in the program refuses the name") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "const v = make(1);\n"
        "v.x = 'hi';\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.fieldAudit.refusals.count("written as string") == 1);
    CHECK(r.provenFieldReads.empty());
    CHECK(r.fieldAudit.numberFieldReads == 5);
    CHECK(r.fieldAudit.refusedByAudit >= 4);
}

TEST_CASE("the audit's unit is the property name, not the class") {
    const auto inferred = infer(
        "class W { constructor() { this.x = 0; } }\n"
        "function make(n) { const w = new W(); w.x = n; return w; }\n"
        "function read(w) { let s = w.x; for (let i = 0; i < 2; i++) s = w.x; return s; }\n"
        "const other = { x: 1 };\n"
        "other.x = 'not a number';\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.fieldAudit.refusals.count("written as string") == 1);
    CHECK(r.provenFieldReads.empty());
}

TEST_CASE("one computed write with unknown key and unknown value stands every name down") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "function poke(o, k, val) { o[k] = val; }\n"
        "const v = make(1);\n"
        "poke(v, 'x', 'hi');\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.fieldAudit.namesLocallyClean == 1);
    CHECK(r.fieldAudit.globalRefusals.size() == 1);
    CHECK(r.fieldAudit.computedSites >= 1);
    CHECK(r.fieldAudit.computedRefuted >= 1);
    CHECK(r.fieldAudit.residue.size() >= 1);
    CHECK(r.provenFieldReads.empty());
}

TEST_CASE("a computed write with a number key cannot name a non-numeric field") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "function fill(o) { for (let i = 0; i < 4; i++) o[i] = 'text'; }\n"
        "const v = make(1);\n"
        "fill([]);\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.provenFieldReads.size() >= 3);
}

TEST_CASE("a computed write with number value is harmless") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function setDyn(o, k) { o[k] = 123; }\n"
        "function read(v) { return v.x; }\n"
        "const v = new V();\n"
        "setDyn(v, 'x');\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.namesClean == 1);
}

TEST_CASE("a computed write with literal string set resolves names") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; this.y = 0; } }\n"
        "function setNamed(o, pick) {\n"
        "  const key = pick ? 'x' : 'y';\n"
        "  o[key] = 42;\n"
        "}\n"
        "const v = new V();\n"
        "setNamed(v, true);\n"
        "console.log(v.x + v.y);\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.namesClean == 2);
}

TEST_CASE("a computed delete with literal string set refutes only named fields") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; this.y = 0; } }\n"
        "function delNamed(o, pick) {\n"
        "  const key = pick ? 'x' : 'y';\n"
        "  delete o[key];\n"
        "}\n"
        "const v = new V();\n"
        "console.log(v.x);\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.refusals.count("deleted") >= 1);
}

TEST_CASE("an accessor over the name refuses the field, not the write") {
    const auto inferred = infer(
        "class V {\n"
        "  constructor() { this._x = 0; }\n"
        "  get x() { return this._x; }\n"
        "}\n"
        "function make(n) { const v = new V(); v._x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.fieldAudit.numberFieldReads == 3);
    CHECK(r.provenFieldReads.size() >= 1);
    CHECK(r.fieldAudit.refusedByAudit == 0);
}

TEST_CASE("an accessor refuses the field read on classes with the accessor") {
    const auto inferred = infer(
        "class Base { constructor() { this.x = 0; } }\n"
        "class Derived extends Base {\n"
        "  get x() { return 5; }\n"
        "  set x(v) { this._v = v; }\n"
        "}\n"
        "function make(n) { const d = new Derived(); d.x = n; return d; }\n"
        "function read(d) { let s = d.x; for (let i = 0; i < 2; i++) s = d.x; return s; }\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.fieldAudit.refusedByClass >= 1);
    CHECK(r.provenFieldReads.size() == 1);
}

TEST_CASE("a numeric compound assignment preserves the field's proof") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "const v = make(1);\n"
        "v.x += 5;\n"
        "v.x++;\n"
        "v.x *= 2;\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.provenFieldReads.size() >= 6);
}

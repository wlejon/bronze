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

TEST_CASE("one computed write with unknown key and unknown value stands its class down") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "function poke(o, k, val) { o[k] = val; }\n"
        "const v = make(1);\n"
        "poke(v, 'x', 'hi');\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    // The obligation, unchanged: `read` may not spend a Number claim on a slot
    // this write can put a string in. What carries it is the receiver — `poke`
    // is called once, with an object `make` was watched building, so the write
    // is proven to reach V and proven not to reach anything else.
    CHECK(r.provenFieldReads.empty());
    CHECK(r.fieldAudit.computedSites >= 1);
    CHECK(r.fieldAudit.computedRefuted >= 1);
    CHECK(r.fieldAudit.residue.size() >= 1);
    CHECK(r.fieldAudit.classScopedRefusals.size() == 1);
    CHECK(r.fieldAudit.classScopedRefusals[0].cls == "V{x}");
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.namesLocallyClean == 1);
}

// ---- how far an unanalyzable computed write's refusal reaches ---------------

TEST_CASE("a computed write through a watched receiver refuses that class alone") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "class W { constructor() { this.x = 0; } }\n"
        "function readV(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "function readW(w) { let s = w.x; for (let i = 0; i < 2; i++) s = w.x; return s; }\n"
        "const kk = ['x'][0];\n"
        "const v = new V();\n"
        "v[kk] = 'hi';\n"
        "const w = new W();\n"
        "w.x = 3;\n"
        "console.log(readV(v));\n"
        "console.log(readW(w));\n");
    const auto& r = *inferred.result;
    // Not a program-wide answer, and `x` carries no refusal of its own: what
    // stands V's reads down is the class the write was proven to reach.
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.fieldAudit.classScopedRefusals.size() == 1);
    CHECK(r.fieldAudit.classScopedRefusals[0].cls == "V{x}");
    // W's three reads keep the claim; V's do not.
    CHECK(r.fieldAudit.refusedByAudit >= 3);
    CHECK(r.provenFieldReads.size() >= 3);
}

TEST_CASE("a computed write's refusal follows the receiver's extends family") {
    const auto inferred = infer(
        "class Base {\n"
        "  constructor() { this.x = 0; }\n"
        "  sum() { let s = this.x; for (let i = 0; i < 2; i++) s = this.x; return s; }\n"
        "}\n"
        "class Derived extends Base { constructor() { super(); this.z = 1; } }\n"
        "const kk = ['x'][0];\n"
        "const d = new Derived();\n"
        "d[kk] = 'hi';\n"
        "console.log(d.sum());\n");
    const auto& r = *inferred.result;
    // The write reached a Derived. `sum` runs with `this` typed Base, and a
    // Base-typed receiver is exactly what a Derived arrives as, so the refusal
    // has to cover the read too — the layouts nest and the intervals overlap.
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.classScopedRefusals.size() == 1);
    CHECK(r.provenFieldReads.empty());
    CHECK(r.fieldAudit.refusedByAudit >= 1);
}

TEST_CASE("a computed write through a receiver typed as an array costs no class") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "const kk = ['x'][0];\n"
        "const arr = [1, 2, 3];\n"
        "arr[kk] = 'hi';\n"
        "const v = new V();\n"
        "v.x = 2;\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    // An array is not an instance of any declared class, so the write reaches
    // nothing a layout answers for. `x` is not a name a numeric key could be.
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.classScopedRefusals.empty());
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.provenFieldReads.size() >= 3);
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
    // The counter says how many; this says which, and by which of the four
    // conditions — the accessor one here, named on the class that declares it.
    CHECK(r.fieldAudit.classRefusedSites.count(
              "Derived.x: an accessor of that name on the prototype chain") == 1);
}

TEST_CASE("a field a method installs is refused by name, not by the accessor reason") {
    const auto inferred = infer(
        "class L {\n"
        "  constructor() { this.x = 0; }\n"
        "  late() { this.y = 1; }\n"
        "}\n"
        "function make(n) { const l = new L(); l.late(); l.y = n; return l; }\n"
        "function read(l) { let s = l.y; for (let i = 0; i < 2; i++) s = l.y; return s; }\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.refusedByClass >= 1);
    CHECK(r.fieldAudit.classRefusedSites.count(
              "L.y: not installed by the construction sequence") == 1);
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

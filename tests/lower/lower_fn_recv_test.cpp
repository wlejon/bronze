// The function-receiver hint (`src/lower/lower_infer.cpp`
// `functionBindingReceiver`, printed as `, fn-recv`): which property-read and
// method-call sites the backend gives its function-statics arm to.
//
// The bit is a HINT, not a proof. Nothing here claims the receiver IS a
// function at run time — the binding could be reassigned, and the emitted code
// tests the receiver's flags regardless. What the bit decides is only whether
// the arm is EMITTED, so a wrong answer costs one of two things and never
// correctness: a site that could have cached a static and did not, or an arm
// emitted where it can never fire. The second is the expensive one, which is
// why the negative cases below are as load-bearing as the positive ones —
// emitting the arm at `this.x` and at parameter receivers is what made an
// earlier, unhinted version slower than no arm at all.
//
// Asserted on the printed module because `, fn-recv` is what the claim IS:
// the field exists to be read by the backend, and no other output
// distinguishes a site that carries it from one that does not.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;

namespace {

std::string lowerToText(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = inferAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    return il::print(*mod);
}

size_t hints(const std::string& il) {
    size_t n = 0;
    size_t at = il.find("fn-recv");
    while (at != std::string::npos) {
        ++n;
        at = il.find("fn-recv", at + 1);
    }
    return n;
}

}  // namespace

TEST_CASE("a class binding's static read is hinted") {
    const auto il = lowerToText(
        "class Cfg { static LIMIT = 3; }\n"
        "function read() { return Cfg.LIMIT; }\n"
        "console.log(read());\n");
    CHECK(hints(il) == 1);
}

TEST_CASE("a function declaration's own name is hinted") {
    const auto il = lowerToText(
        "function f() { return 1; }\n"
        "f.tag = 'x';\n"
        "function read() { return f.tag; }\n"
        "console.log(read());\n");
    CHECK(hints(il) >= 1);
}

TEST_CASE("`this` and a parameter receiver are not hinted") {
    // The two receivers that dominate a math kernel's inner loop. Neither can
    // be a class binding, and hinting either put the arm in the hottest code
    // in three.js for a path it can never take.
    const auto il = lowerToText(
        "class P {\n"
        "  constructor(v) { this.v = v; }\n"
        "  read(other) { return this.v + other.v; }\n"
        "}\n"
        "console.log(new P(1).read(new P(2)));\n");
    CHECK(hints(il) == 0);
}

TEST_CASE("a local holding an instance is not hinted") {
    const auto il = lowerToText(
        "class P { constructor() { this.v = 1; } }\n"
        "const p = new P();\n"
        "console.log(p.v);\n");
    CHECK(hints(il) == 0);
}

TEST_CASE("a constructor global is hinted and a namespace global is not") {
    // 21.3 makes `Math` an ordinary object, never a function object, so the
    // arm could never fire on it — and `Math.max`/`Math.min` are the most-read
    // receivers in three.js's math classes. `Object` is a function (20.1.1)
    // and its members really are statics.
    const auto objectIl = lowerToText("function r(o) { return Object.getPrototypeOf(o); }\n"
                                      "console.log(typeof r({}));\n");
    CHECK(hints(objectIl) >= 1);

    const auto mathIl = lowerToText("function r(a, b) { return Math.max(a, b) + Math.min(a, b); }\n"
                                    "console.log(r(1, 2));\n");
    CHECK(hints(mathIl) == 0);
}

TEST_CASE("a table-answered global is not hinted even though it is a function") {
    // `Number` IS a function object and `EPSILON` IS in its statics box, so
    // neither of the two tests above explains this one. What decides it is
    // that `rtGlobalConstructorMember` answers for the receiver, which makes
    // the runtime refuse to install an entry — an armed site would therefore
    // pay its loads and its compare and fall through on every read. This
    // exact expression, in `Quaternion.slerp`, was the hottest statics read in
    // the three.js math benchmark and could never hit.
    const auto numberIl = lowerToText("function r(x) { return x + Number.EPSILON; }\n"
                                      "console.log(r(1));\n");
    CHECK(hints(numberIl) == 0);

    // Two more from the same table, both reached through a member expression
    // rather than the call recognition, so the hint really is what is asked.
    const auto arrayIl = lowerToText("function r(x) { const f = Array.isArray; return f(x); }\n"
                                     "console.log(r([]));\n");
    CHECK(hints(arrayIl) == 0);

    // And the positive control in the same shape: `Object.keys` is an own data
    // property of a real box, so this one is hinted.
    const auto objectIl = lowerToText("function r() { const f = Object.keys; return f({}); }\n"
                                      "console.log(r().length);\n");
    CHECK(hints(objectIl) == 1);
}

TEST_CASE("a `prototype` read is not hinted") {
    // Answered from the FunctionHeader's own slot, before the statics box is
    // consulted at all — and the backend already has an inline branch for a
    // function receiver's `prototype`, so an arm here asks a question that is
    // answered elsewhere and can never hit. three.js r160 writes exactly this
    // inside `Vector3`, `Euler` and `Matrix4`, which is what made it worth a
    // case of its own.
    const auto il = lowerToText("class Vec { constructor() { Vec.prototype.isVec = true; } }\n"
                                "console.log(typeof new Vec());\n");
    CHECK(hints(il) == 0);

    // The same class binding, a key that IS a static: still hinted, so the
    // exclusion is the key's and not the receiver's.
    const auto statIl = lowerToText("class Vec { static UP = 1; }\n"
                                    "function r() { return Vec.UP; }\n"
                                    "console.log(r());\n");
    CHECK(statIl.find("fn-recv") != std::string::npos);
}

TEST_CASE("a chained member receiver is not hinted") {
    // Only a bare identifier can be a declaration's binding. `a.b.c` reads
    // `c` off whatever `a.b` produced, which is not a name lowering resolved.
    const auto il = lowerToText(
        "class Cfg { static inner = { LIMIT: 3 }; }\n"
        "function read() { return Cfg.inner.LIMIT; }\n"
        "console.log(read());\n");
    // `Cfg.inner` is hinted; `.LIMIT` off its result is not.
    CHECK(hints(il) == 1);
}

TEST_CASE("a static method call is hinted on the call site") {
    const auto il = lowerToText(
        "class Cfg { static make() { return 7; } }\n"
        "function call() { return Cfg.make(); }\n"
        "console.log(call());\n");
    CHECK(hints(il) >= 1);
}

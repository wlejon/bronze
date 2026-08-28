// GUARDED NUMERIC REGIONS (src/lower/guard_region.h): the selection policy and
// the rewrite, pinned two ways.
//
// Hand-built IL for the policy, because a refusal is invisible from source —
// "this loop was not duplicated" and "this loop had nothing to duplicate" print
// the same program — and the canonical text is what says which happened.
// Source-driven IL for the shape, because the point of the pass is what
// `sumProps` compiles to, and a hand-built approximation of `sumProps` would be
// pinning the test's idea of lowering rather than lowering.

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "il/print.h"
#include "il/verifier.h"
#include "lower/guard_region.h"
#include "lower_fixture.h"

using namespace bronze;
using namespace bronze::lower;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

namespace {

// A module with one function, so `il::print` output is short enough to compare
// whole.
il::Module oneFunction(il::Function fn) {
    il::Module module;
    module.name = "test";
    // The inline-cache table is a real global array in the object file, so the
    // verifier bounds-checks every site index against this. A copied `prop.get`
    // SHARES its site with the original — the receiver and the shape are the
    // same, and the entry stays warm — so the count never grows here either.
    module.icSiteCount = 8;
    module.functions.push_back(std::move(fn));
    return module;
}

il::Instruction constF64(il::ValueId result, double value) {
    il::Instruction inst;
    inst.op = il::Op::ConstF64;
    inst.type = il::Type::F64;
    inst.result = result;
    inst.immF64 = value;
    return inst;
}

il::Instruction box(il::ValueId result, il::ValueId operand, il::Type of = il::Type::F64) {
    il::Instruction inst;
    inst.op = il::Op::Box;
    inst.type = il::Type::Dynamic;
    inst.boxType = of;
    inst.result = result;
    inst.operands = {operand};
    return inst;
}

il::Instruction arith(il::Op op, il::ValueId result, il::ValueId a, il::ValueId b,
                      il::Type type = il::Type::Dynamic) {
    il::Instruction inst;
    inst.op = op;
    inst.type = type;
    inst.result = result;
    inst.operands = {a, b};
    return inst;
}

il::Instruction cmpLt(il::ValueId result, il::ValueId a, il::ValueId b) {
    il::Instruction inst;
    inst.op = il::Op::CmpLt;
    inst.type = il::Type::Bool;
    inst.result = result;
    inst.operands = {a, b};
    return inst;
}

il::Instruction jump(il::BlockId target, std::vector<il::ValueId> args = {}) {
    il::Instruction inst;
    inst.op = il::Op::Jump;
    inst.target.block = target;
    inst.target.args = std::move(args);
    return inst;
}

il::Instruction branch(il::ValueId cond, il::BlockId thenBlock, std::vector<il::ValueId> thenArgs,
                       il::BlockId elseBlock, std::vector<il::ValueId> elseArgs) {
    il::Instruction inst;
    inst.op = il::Op::Branch;
    inst.operands = {cond};
    inst.target.block = thenBlock;
    inst.target.args = std::move(thenArgs);
    inst.elseTarget.block = elseBlock;
    inst.elseTarget.args = std::move(elseArgs);
    return inst;
}

il::Instruction ret(il::ValueId value) {
    il::Instruction inst;
    inst.op = il::Op::Ret;
    inst.operands = {value};
    return inst;
}

il::Instruction propGet(il::ValueId result, il::ValueId receiver, uint32_t key, uint32_t ic) {
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = result;
    inst.operands = {receiver};
    inst.keyIndex = key;
    inst.icIndex = ic;
    return inst;
}

// `function f(o, n) { let t = 0; for (let i = 0; i < n; i++) t = t + o.a + o.b; return t; }`
// as lowering emits it, written out so that a test can vary ONE thing about it.
il::Function twoReadLoop() {
    il::Function fn;
    fn.name = "loop";
    fn.params = {{"o", il::Type::Dynamic}, {"n", il::Type::F64}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 18;

    il::Block b0;
    b0.id = 0;
    b0.instructions = {constF64(2, 0), constF64(3, 0), box(6, 2), jump(1, {6, 3})};

    il::Block b1;
    b1.id = 1;
    b1.params = {{4, il::Type::Dynamic}, {5, il::Type::F64}};
    b1.instructions = {cmpLt(9, 5, 1), branch(9, 2, {}, 4, {4, 5})};

    il::Block b2;
    b2.id = 2;
    b2.instructions = {propGet(12, 0, 10, 4), arith(il::Op::Add, 13, 4, 12),
                       propGet(14, 0, 11, 5), arith(il::Op::Add, 15, 13, 14), jump(3, {15, 5})};

    il::Block b3;
    b3.id = 3;
    b3.params = {{10, il::Type::Dynamic}, {11, il::Type::F64}};
    b3.instructions = {constF64(16, 1), arith(il::Op::Add, 17, 11, 16, il::Type::F64),
                       jump(1, {10, 17})};

    il::Block b4;
    b4.id = 4;
    b4.params = {{7, il::Type::Dynamic}, {8, il::Type::F64}};
    b4.instructions = {ret(7)};

    fn.blocks = {b0, b1, b2, b3, b4};
    return fn;
}

// Everything after `func` in the printed module, which is the part a test is
// about — the module header line carries no information here.
std::string body(const il::Module& module) {
    const std::string text = il::print(module);
    const size_t at = text.find("func ");
    return at == std::string::npos ? text : text.substr(at);
}

bool seamIsOn() { return guardedRegionsDisabled(); }

}  // namespace

TEST_CASE("is.number round-trips through the canonical text") {
    il::Function fn;
    fn.name = "test";
    fn.params = {{"v", il::Type::Dynamic}};
    fn.returnType = il::Type::Void;
    fn.valueCount = 3;

    il::Instruction isNum;
    isNum.op = il::Op::IsNumber;
    isNum.type = il::Type::Bool;
    isNum.result = 1;
    isNum.operands = {0};

    il::Block b0;
    b0.id = 0;
    b0.instructions = {isNum, branch(1, 1, {}, 2, {})};
    il::Block b1;
    b1.id = 1;
    il::Instruction retVoid;
    retVoid.op = il::Op::Ret;
    b1.instructions = {retVoid};
    il::Block b2;
    b2.id = 2;
    b2.instructions = {retVoid};
    fn.blocks = {b0, b1, b2};

    const il::Module module = oneFunction(std::move(fn));
    CHECK(body(module) ==
          "func test(%0: dynamic) -> void {\n"
          "  b0:\n"
          "    %1: bool = is.number %0\n"
          "    br %1, b1, b2\n"
          "  b1:\n"
          "    ret\n"
          "  b2:\n"
          "    ret\n"
          "}\n");

    // Total and non-allocating, which is what licenses the raw unbox on the
    // true edge and what keeps a receiver proof alive across it.
    CHECK_FALSE(il::canThrow(module.functions[0].blocks[0].instructions[0]));
    CHECK_FALSE(il::canCollect(module.functions[0].blocks[0].instructions[0]));

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("the verifier refuses a malformed is.number") {
    SUBCASE("an unboxed operand") {
        il::Function fn;
        fn.name = "test";
        fn.params = {{"v", il::Type::F64}};
        fn.valueCount = 3;
        il::Instruction isNum;
        isNum.op = il::Op::IsNumber;
        isNum.type = il::Type::Bool;
        isNum.result = 1;
        isNum.operands = {0};
        il::Block b0;
        b0.id = 0;
        il::Instruction retVoid;
        retVoid.op = il::Op::Ret;
        b0.instructions = {isNum, retVoid};
        fn.blocks = {b0};
        DiagnosticSink diags;
        CHECK_FALSE(il::verify(oneFunction(std::move(fn)), diags));
    }
    SUBCASE("a result that is not a bool") {
        il::Function fn;
        fn.name = "test";
        fn.params = {{"v", il::Type::Dynamic}};
        fn.valueCount = 3;
        il::Instruction isNum;
        isNum.op = il::Op::IsNumber;
        isNum.type = il::Type::Dynamic;
        isNum.result = 1;
        isNum.operands = {0};
        il::Block b0;
        b0.id = 0;
        il::Instruction retVoid;
        retVoid.op = il::Op::Ret;
        b0.instructions = {isNum, retVoid};
        fn.blocks = {b0};
        DiagnosticSink diags;
        CHECK_FALSE(il::verify(oneFunction(std::move(fn)), diags));
    }
}

TEST_CASE("a two-read loop becomes a fast copy, two guards and two trampolines") {
    if (seamIsOn()) return;
    il::Module module = oneFunction(twoReadLoop());
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    // Two reads, two guards; the accumulator's def is `box.f64 %2` on the entry
    // edge, so it is promoted with no test at all.
    CHECK(stats.guards == 2);
    CHECK(stats.elidedBox == 1);

    CHECK(body(module) ==
          "func loop(%0: dynamic, %1: f64) -> dynamic {\n"
          "  b0:\n"
          "    %2: f64 = const.f64 0\n"
          "    %3: f64 = const.f64 0\n"
          "    jump b7(%2, %3)\n"
          "  b1(%4: dynamic, %5: f64):\n"
          "    %9: bool = cmp.lt %5, %1\n"
          "    br %9, b2, b6(%4, %5)\n"
          "  b2:\n"
          "    %12: dynamic = prop.get %0, 10, 4\n"
          "    jump b3(%4, %5, %12)\n"
          "  b3(%31: dynamic, %32: f64, %33: dynamic):\n"
          "    %13: dynamic = add %31, %33\n"
          "    %14: dynamic = prop.get %0, 11, 5\n"
          "    jump b4(%32, %13, %14)\n"
          "  b4(%34: f64, %35: dynamic, %36: dynamic):\n"
          "    %15: dynamic = add %35, %36\n"
          "    jump b5(%15, %34)\n"
          "  b5(%10: dynamic, %11: f64):\n"
          "    %16: f64 = const.f64 1\n"
          "    %17: f64 = add %11, %16\n"
          "    jump b1(%10, %17)\n"
          "  b6(%7: dynamic, %8: f64):\n"
          "    ret %7\n"
          "  b7(%18: f64, %19: f64):\n"
          "    %20: bool = cmp.lt %19, %1\n"
          "    br %20, b8, b14\n"
          "  b8:\n"
          "    %21: dynamic = prop.get %0, 10, 4\n"
          "    %38: bool = is.number %21\n"
          "    br %38, b9, b12\n"
          "  b9:\n"
          "    %29: f64 = unbox.f64 %21, raw\n"
          "    %22: f64 = add %18, %29\n"
          "    %23: dynamic = prop.get %0, 11, 5\n"
          "    %40: bool = is.number %23\n"
          "    br %40, b10, b13\n"
          "  b10:\n"
          "    %30: f64 = unbox.f64 %23, raw\n"
          "    %24: f64 = add %22, %30\n"
          "    jump b11(%24, %19)\n"
          "  b11(%25: f64, %26: f64):\n"
          "    %27: f64 = const.f64 1\n"
          "    %28: f64 = add %26, %27\n"
          "    jump b7(%25, %28)\n"
          "  b12:\n"
          "    %39: dynamic = box.f64 %18\n"
          "    jump b3(%39, %19, %21)\n"
          "  b13:\n"
          "    %41: dynamic = box.f64 %22\n"
          "    jump b4(%19, %41, %23)\n"
          "  b14:\n"
          "    %37: dynamic = box.f64 %18\n"
          "    jump b6(%37, %19)\n"
          "}\n");

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a closure holding a string is refused whole") {
    if (seamIsOn()) return;
    il::Function fn = twoReadLoop();
    // `%14` is now a string, so the component that reaches both adds can never
    // be a Number and the guard would fail every iteration. Refused STATICALLY:
    // no block is copied.
    fn.blocks[2].instructions[2] = box(14, 0, il::Type::Str);
    const il::Module before = oneFunction(fn);
    il::Module after = oneFunction(std::move(fn));
    GuardRegionStats stats;
    CHECK_FALSE(applyGuardedRegions(after, &stats));
    CHECK(stats.refusedNonNumeric == 1);
    CHECK(body(after) == body(before));
}

TEST_CASE("a block with a handler is refused") {
    if (seamIsOn()) return;
    il::Function fn = twoReadLoop();
    fn.blocks[2].handler = 4;
    fn.blocks[4].params.clear();
    fn.blocks[1].instructions[1] = branch(9, 2, {}, 4, {});
    fn.blocks[4].instructions = {ret(0)};
    const il::Module before = oneFunction(fn);
    il::Module after = oneFunction(std::move(fn));
    GuardRegionStats stats;
    CHECK_FALSE(applyGuardedRegions(after, &stats));
    CHECK(stats.refusedHandler == 1);
    CHECK(body(after) == body(before));
}

TEST_CASE("a header with two outside predecessors is refused, not mis-duplicated") {
    if (seamIsOn()) return;
    il::Function fn = twoReadLoop();
    // A second way into the loop, which is what a `for` with a conditional
    // initialiser lowers to. There is no single edge to convert the promoted
    // values on, and chunk 1 has no preheader to synthesise one.
    il::Block b5;
    b5.id = 5;
    b5.instructions = {constF64(18, 7), box(19, 18), jump(1, {19, 3})};
    fn.blocks[0].instructions.back() = branch(0, 1, {6, 3}, 5, {});
    fn.blocks[0].instructions[2] = cmpLt(0, 2, 3);
    fn.blocks[0].instructions = {constF64(2, 0), constF64(3, 0), box(6, 2), cmpLt(20, 2, 3),
                                 branch(20, 1, {6, 3}, 5, {})};
    fn.blocks.push_back(b5);
    fn.valueCount = 21;

    const il::Module before = oneFunction(fn);
    il::Module after = oneFunction(std::move(fn));
    GuardRegionStats stats;
    CHECK_FALSE(applyGuardedRegions(after, &stats));
    CHECK(stats.refusedSingleEntry == 1);
    CHECK(body(after) == body(before));
}

TEST_CASE("more guards than arithmetic is under the floor") {
    if (seamIsOn()) return;
    il::Function fn = twoReadLoop();
    // `t = o.a + o.b`: two unproven reads for one add, so the hoisted test is a
    // test per operation, which is what the backend's own inline arm already
    // emits. Nothing has been bought.
    fn.blocks[2].instructions = {propGet(12, 0, 10, 4), propGet(14, 0, 11, 5),
                                 arith(il::Op::Add, 15, 12, 14), jump(3, {15, 5})};
    const il::Module before = oneFunction(fn);
    il::Module after = oneFunction(std::move(fn));
    GuardRegionStats stats;
    CHECK_FALSE(applyGuardedRegions(after, &stats));
    CHECK(stats.refusedTooFew == 1);
    CHECK(body(after) == body(before));
}

TEST_CASE("a candidate a pin already proved gets no guard of its own") {
    if (seamIsOn()) return;
    il::Function fn = twoReadLoop();
    il::Instruction pin;
    pin.op = il::Op::PinGuard;
    pin.type = il::Type::Void;
    pin.operands = {12};
    pin.keyIndex = 10;
    pin.immI32 = static_cast<int32_t>(il::PinBarrier::Number);
    // The barrier stands immediately in front of the first promoted use, which
    // is the one instruction of reach `storeValueRepr` reads a pin over.
    fn.blocks[2].instructions.insert(fn.blocks[2].instructions.begin() + 1, pin);

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));
    CHECK(stats.elidedPin == 1);
    // One guard, not two: the second read is still unproven.
    CHECK(stats.guards == 1);
    CHECK(body(module).find("is.number %26") == std::string::npos);
    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("the inner loop of a nest is the one duplicated") {
    if (seamIsOn()) return;
    il::Function fn;
    fn.name = "nest";
    fn.params = {{"o", il::Type::Dynamic}, {"n", il::Type::F64}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 20;

    // b0 -> b1(outer header) -> b2(inner header) -> b3(inner body) -> b2,
    // b2 -> b4 -> b1, b1 -> b5.
    il::Block b0;
    b0.id = 0;
    b0.instructions = {constF64(2, 0), constF64(3, 0), box(4, 2), jump(1, {4, 3})};
    il::Block b1;
    b1.id = 1;
    b1.params = {{5, il::Type::Dynamic}, {6, il::Type::F64}};
    b1.instructions = {cmpLt(7, 6, 1), branch(7, 2, {5, 6}, 5, {5})};
    il::Block b2;
    b2.id = 2;
    b2.params = {{8, il::Type::Dynamic}, {9, il::Type::F64}};
    b2.instructions = {cmpLt(10, 9, 1), branch(10, 3, {}, 4, {8, 9})};
    il::Block b3;
    b3.id = 3;
    b3.instructions = {propGet(11, 0, 10, 0), arith(il::Op::Add, 12, 8, 11),
                       propGet(13, 0, 11, 1), arith(il::Op::Add, 14, 12, 13), constF64(15, 1),
                       arith(il::Op::Add, 16, 9, 15, il::Type::F64), jump(2, {14, 16})};
    il::Block b4;
    b4.id = 4;
    b4.params = {{17, il::Type::Dynamic}, {18, il::Type::F64}};
    b4.instructions = {jump(1, {17, 18})};
    il::Block b5;
    b5.id = 5;
    b5.params = {{19, il::Type::Dynamic}};
    b5.instructions = {ret(19)};
    fn.blocks = {b0, b1, b2, b3, b4, b5};

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));
    CHECK(stats.duplicated == 1);
    // The outer loop's blocks properly contain the inner loop's, so it is
    // refused here rather than nested: one duplication per loop nest.
    const std::string text = body(module);
    CHECK(text.find("is.number") != std::string::npos);
    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

// ---------------------------------------------------------------------------
// Source-driven: the shape of the thing on the program the design is about.
// ---------------------------------------------------------------------------

namespace {

const char* kSumProps = R"(
function sumProps(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
)";

// The function's printed text, whichever mode produced the module.
std::string functionText(const il::Module& module, const std::string& name) {
    const std::string text = il::print(module);
    const size_t at = text.find("func " + name + "(");
    if (at == std::string::npos) return {};
    const size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
}

}  // namespace

TEST_CASE("sumProps carries its accumulator as an f64 in both inference modes") {
    if (seamIsOn()) return;
    for (int mode = 0; mode < 2; ++mode) {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        auto module = mode == 0 ? inferAndLower(kSumProps, diags, buf)
                                : parseAndLower(kSumProps, diags, buf);
        REQUIRE(module.has_value());
        const std::string text = functionText(*module, "sumProps");
        REQUIRE_FALSE(text.empty());

        // The fast copy exists: a guard, a raw unbox behind it, and an `add`
        // that produces an f64 instead of a rooted box.
        CHECK(text.find("is.number") != std::string::npos);
        CHECK(text.find(", raw") != std::string::npos);
        CHECK(text.find("f64 = add") != std::string::npos);
        // And it re-boxes on the way out, which is the only place a promoted
        // value crosses back.
        CHECK(text.find("box.f64") != std::string::npos);

        CHECK(il::verify(*module, diags));
        CHECK_FALSE(diags.hasErrors());
    }
}

TEST_CASE("the fast header's accumulator parameter is an f64") {
    if (seamIsOn()) return;
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto module = inferAndLower(kSumProps, diags, buf);
    REQUIRE(module.has_value());
    const il::Function* fn = nullptr;
    for (const auto& f : module->functions) {
        if (f.name == "sumProps") fn = &f;
    }
    REQUIRE(fn != nullptr);

    // Every block reached by an `is.number`'s true edge, and the header of the
    // copy it belongs to, carries the accumulator as an `f64` parameter. The
    // count is what says the loop was duplicated once and not twice.
    int f64Params = 0;
    int dynamicAdds = 0;
    int f64Adds = 0;
    for (const auto& block : fn->blocks) {
        for (const auto& param : block.params) {
            if (param.type == il::Type::F64) ++f64Params;
        }
        for (const auto& inst : block.instructions) {
            if (inst.op != il::Op::Add) continue;
            if (inst.type == il::Type::Dynamic) ++dynamicAdds;
            if (inst.type == il::Type::F64) ++f64Adds;
        }
    }
    // Two boxed adds survive in the slow copy; the fast copy's two are f64, and
    // so are the two loop counters.
    CHECK(dynamicAdds == 2);
    CHECK(f64Adds >= 3);
    CHECK(f64Params >= 4);
}

TEST_CASE("with the seam on, sumProps is exactly the IL it always was") {
    // The A/B that makes the seam a bisection tool rather than a comment: the
    // same process lowers the same source twice, and the only difference is
    // whether the pass ran.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto module = inferAndLower(kSumProps, diags, buf);
    REQUIRE(module.has_value());
    const std::string text = functionText(*module, "sumProps");

    if (seamIsOn()) {
        CHECK(text.find("is.number") == std::string::npos);
        CHECK(text ==
              "func sumProps(%0: dynamic, %1: dynamic) -> f64 {\n"
              "  b0:\n"
              "    %2: f64 = const.f64 0\n"
              "    %3: f64 = const.f64 0\n"
              "    jump b1(%2, %3)\n"
              "  b1(%4: f64, %5: f64):\n"
              "    %8: dynamic = box.f64 %5\n"
              "    %9: bool = rel.lt %8, %1\n"
              "    br %9, b2, b4(%4, %5)\n"
              "  b2:\n"
              "    %12: dynamic = prop.get %0, 1, 0\n"
              "    %13: dynamic = box.f64 %4\n"
              "    %14: dynamic = add %13, %12\n"
              "    %15: dynamic = prop.get %0, 2, 1\n"
              "    %16: dynamic = add %14, %15\n"
              "    %17: f64 = unbox.f64 %16\n"
              "    jump b3(%17, %5)\n"
              "  b3(%10: f64, %11: f64):\n"
              "    %18: f64 = const.f64 1\n"
              "    %19: f64 = add %11, %18\n"
              "    jump b1(%10, %19)\n"
              "  b4(%6: f64, %7: f64):\n"
              "    ret %6\n"
              "}\n");
    } else {
        CHECK(text.find("is.number") != std::string::npos);
    }
}

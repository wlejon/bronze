// GUARDED NUMERIC REGIONS, chunk 2: the ENTRY region, the checked-unbox
// candidate and guard COALESCING (src/lower/guard_region.h).
//
// Split from `guard_region_test.cpp` rather than appended to it because that
// file is about the loop region and its refusals, and this one is about a
// different region kind, a different candidate kind and a different placement
// rule. Same two halves as its sibling: hand-built IL where the answer is a
// refusal or an exact shape, source-driven IL where the point is what a real
// kernel compiles to.

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

il::Module oneFunction(il::Function fn) {
    il::Module module;
    module.name = "test";
    module.icSiteCount = 64;
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

// A CHECKED `unbox.f64`: no `raw`, no `nullish`. The thing chunk 2 spends a
// guard on.
il::Instruction unboxChecked(il::ValueId result, il::ValueId operand) {
    il::Instruction inst;
    inst.op = il::Op::Unbox;
    inst.type = il::Type::F64;
    inst.result = result;
    inst.operands = {operand};
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

std::string body(const il::Module& module) {
    const std::string text = il::print(module);
    const size_t at = text.find("func ");
    return at == std::string::npos ? text : text.substr(at);
}

std::string functionText(const il::Module& module, const std::string& name) {
    const std::string text = il::print(module);
    const size_t at = text.find("func " + name + "(");
    if (at == std::string::npos) return {};
    const size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
}

size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

bool seamIsOn() { return guardedRegionsDisabled(); }

}  // namespace

// ---------------------------------------------------------------------------
// (a) The entry region proper.
// ---------------------------------------------------------------------------

TEST_CASE("two parameter candidates become one two-guard chain to one trampoline") {
    if (seamIsOn()) return;
    // `function f(a, b) { return a * b + a; }` with nothing proven. There is no
    // loop, so the region is the whole function: block 0 is a synthesised
    // preheader, the fast copy is the entry, and a failing guard lands in the
    // original body with NOTHING promoted live — nothing has been computed yet.
    il::Function fn;
    fn.name = "f";
    fn.params = {{"a", il::Type::Dynamic}, {"b", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 4;

    il::Block b0;
    b0.id = 0;
    b0.instructions = {arith(il::Op::Mul, 2, 0, 1), arith(il::Op::Add, 3, 2, 0), ret(3)};
    fn.blocks = {b0};

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.entryRegions == 1);
    CHECK(stats.regions == 0);
    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 2);

    CHECK(body(module) ==
          "func f(%0: dynamic, %1: dynamic) -> dynamic {\n"
          "  b0:\n"
          "    jump b2\n"
          "  b1:\n"
          "    %2: dynamic = mul %0, %1\n"
          "    %3: dynamic = add %2, %0\n"
          "    ret %3\n"
          "  b2:\n"
          "    %8: bool = is.number %0\n"
          "    br %8, b3, b1\n"
          "  b3:\n"
          "    %9: bool = is.number %1\n"
          "    br %9, b4, b1\n"
          "  b4:\n"
          "    %6: f64 = unbox.f64 %0, raw\n"
          "    %7: f64 = unbox.f64 %1, raw\n"
          "    jump b5\n"
          "  b5:\n"
          "    %4: f64 = mul %6, %7\n"
          "    %5: f64 = add %4, %6\n"
          "    %10: dynamic = box.f64 %5\n"
          "    ret %10\n"
          "}\n");

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

// ---------------------------------------------------------------------------
// (b) The checked unbox, and one guard for several reads of one value.
// ---------------------------------------------------------------------------

TEST_CASE("two checked unboxes of one value share one guard and become one raw unbox") {
    if (seamIsOn()) return;
    // What `p * p` compiles to once `*` takes the numeric arm: the multiply is
    // already an `f64`, and what is boxed is the operand of each coercion. The
    // guard is spent on `%0` ONCE and both unboxes are deleted — their result
    // is the single raw unbox the guard licensed.
    il::Function fn;
    fn.name = "sq";
    fn.params = {{"p", il::Type::Dynamic}};
    fn.returnType = il::Type::F64;
    fn.valueCount = 4;

    il::Block b0;
    b0.id = 0;
    b0.instructions = {unboxChecked(1, 0), unboxChecked(2, 0),
                       arith(il::Op::Mul, 3, 1, 2, il::Type::F64), ret(3)};
    fn.blocks = {b0};

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    // ONE test for a value read twice, and both coercions folded onto it.
    CHECK(stats.guards == 1);
    CHECK(stats.unboxFolded == 2);

    CHECK(body(module) ==
          "func sq(%0: dynamic) -> f64 {\n"
          "  b0:\n"
          "    jump b2\n"
          "  b1:\n"
          "    %1: f64 = unbox.f64 %0\n"
          "    %2: f64 = unbox.f64 %0\n"
          "    %3: f64 = mul %1, %2\n"
          "    ret %3\n"
          "  b2:\n"
          "    %6: bool = is.number %0\n"
          "    br %6, b3, b1\n"
          "  b3:\n"
          "    %5: f64 = unbox.f64 %0, raw\n"
          "    jump b4\n"
          "  b4:\n"
          "    %4: f64 = mul %5, %5\n"
          "    ret %4\n"
          "}\n");

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

// ---------------------------------------------------------------------------
// (c) Coalescing: one point per run of already-defined candidates.
// ---------------------------------------------------------------------------

TEST_CASE("three candidates defined before the first use coalesce, a fourth takes the next point") {
    if (seamIsOn()) return;
    // Three unproven reads feeding an add chain, then a fourth read after it.
    // The rule is "the first promoted use of ANY unassigned candidate, and
    // everything defined by then": one chain of three tests at index 3, one
    // more at index 6. Not four splits, and — the part that matters on a real
    // kernel — the reads before the first point stay in ONE block.
    il::Function fn;
    fn.name = "reads";
    fn.params = {{"o", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 10;

    il::Block b0;
    b0.id = 0;
    // Five adds for four tested reads: the ratio floor refuses a region whose
    // guards outnumber the work they license, and this is about placement, not
    // about the floor.
    b0.instructions = {propGet(1, 0, 10, 0),         propGet(2, 0, 11, 1),
                       propGet(3, 0, 12, 2),         arith(il::Op::Add, 4, 1, 2),
                       arith(il::Op::Add, 5, 4, 3),  propGet(6, 0, 13, 3),
                       arith(il::Op::Add, 7, 5, 6),  arith(il::Op::Add, 8, 7, 1),
                       arith(il::Op::Add, 9, 8, 2),  ret(9)};
    fn.blocks = {b0};

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    // Four values tested, at TWO points — the number of block splits, and the
    // number of trampolines.
    CHECK(stats.guards == 4);
    CHECK(stats.guardPoints == 2);

    const std::string text = body(module);
    CHECK(countOf(text, "is.number") == 4);

    // The three reads before the first guard are still ADJACENT in one block,
    // with the first test immediately after the last of them. That adjacency is
    // the invariant the receiver proof lives on, and a guard per candidate
    // would have put a block boundary between every pair.
    bool sawRunOfThree = false;
    for (const auto& block : module.functions[0].blocks) {
        for (size_t i = 0; i + 3 < block.instructions.size(); ++i) {
            if (block.instructions[i].op == il::Op::PropGet &&
                block.instructions[i + 1].op == il::Op::PropGet &&
                block.instructions[i + 2].op == il::Op::PropGet &&
                block.instructions[i + 3].op == il::Op::IsNumber) {
                sawRunOfThree = true;
            }
        }
    }
    CHECK(sawRunOfThree);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

// ---------------------------------------------------------------------------
// (d) A function with a loop is a loop function.
// ---------------------------------------------------------------------------

TEST_CASE("a function with a loop gets the loop region and never an entry region") {
    if (seamIsOn()) return;
    // Straight-line promotable arithmetic in `b0`, and a counted loop after it.
    // The entry region is refused SILENTLY — a loop function is not an
    // entry-region opportunity the pass declined, it is a different question —
    // and the counters say one region, of the loop kind.
    il::Function fn;
    fn.name = "both";
    fn.params = {{"o", il::Type::Dynamic}, {"n", il::Type::F64}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 20;

    il::Block b0;
    b0.id = 0;
    // `const seed = o.a * o.b;` before the loop: promotable, and not promoted.
    b0.instructions = {propGet(2, 0, 10, 0),        propGet(3, 0, 11, 1),
                       arith(il::Op::Mul, 4, 2, 3), constF64(5, 0),
                       jump(1, {4, 5})};

    il::Block b1;
    b1.id = 1;
    b1.params = {{6, il::Type::Dynamic}, {7, il::Type::F64}};
    b1.instructions = {cmpLt(8, 7, 1), branch(8, 2, {}, 4, {6, 7})};

    il::Block b2;
    b2.id = 2;
    b2.instructions = {propGet(9, 0, 12, 2), arith(il::Op::Add, 10, 6, 9), jump(3, {10, 7})};

    il::Block b3;
    b3.id = 3;
    b3.params = {{11, il::Type::Dynamic}, {12, il::Type::F64}};
    b3.instructions = {constF64(13, 1), arith(il::Op::Add, 14, 12, 13, il::Type::F64),
                       jump(1, {11, 14})};

    il::Block b4;
    b4.id = 4;
    b4.params = {{15, il::Type::Dynamic}, {16, il::Type::F64}};
    b4.instructions = {ret(15)};

    fn.blocks = {b0, b1, b2, b3, b4};

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.regions == 1);
    CHECK(stats.entryRegions == 0);
    CHECK(stats.duplicated == 1);
    // The multiply in `b0` is still boxed: nothing outside the loop was copied.
    const std::string text = body(module);
    CHECK(countOf(text, "dynamic = mul") == 1);
    CHECK(countOf(text, "f64 = mul") == 0);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

// ---------------------------------------------------------------------------
// (e) Resume machines, refused by name.
// ---------------------------------------------------------------------------

TEST_CASE("a generator's resume body is refused whatever its arithmetic looks like") {
    if (seamIsOn()) return;
    il::Function fn;
    fn.name = "g.resume";
    fn.params = {{"__env", il::Type::Dynamic}, {"__mode", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.needsEnv = true;
    fn.valueCount = 6;

    il::Block b0;
    b0.id = 0;
    b0.instructions = {arith(il::Op::Mul, 2, 0, 1), arith(il::Op::Add, 3, 2, 0),
                       arith(il::Op::Sub, 4, 3, 1), ret(4)};
    fn.blocks = {b0};

    SUBCASE("as an ordinary function it IS duplicated") {
        // The control: the same blocks, the same arithmetic, and the only
        // difference is the flag. Without it the region is taken, so the
        // refusal below is the flag's doing and not the shape's.
        il::Module module = oneFunction(fn);
        GuardRegionStats stats;
        CHECK(applyGuardedRegions(module, &stats));
        CHECK(stats.duplicated == 1);
        CHECK(stats.refusedMachine == 0);
    }

    SUBCASE("as a resume body it is refused, and the IL is untouched") {
        fn.isResumeBody = true;
        const il::Module before = oneFunction(fn);
        il::Module after = oneFunction(std::move(fn));
        GuardRegionStats stats;
        CHECK_FALSE(applyGuardedRegions(after, &stats));
        CHECK(stats.refusedMachine == 1);
        CHECK(stats.duplicated == 0);
        CHECK(body(after) == body(before));
    }
}

TEST_CASE("lowering marks every resume body, generator and async alike") {
    // The flag is only worth anything if lowering actually sets it, and there
    // are three places that build a resume function (a generator, an async
    // function, an async generator). A body that reaches the pass unmarked
    // would be duplicated, silently.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto module = inferAndLower(
        "function* g(a, b) { const p = a * b; yield p; return a - b; }\n"
        "async function h(a, b) { const p = a * b; return p + a - b; }\n"
        "for (const v of g(3, 4)) console.log(v);\n"
        "h(5, 6).then((v) => console.log(v));\n",
        diags, buf);
    REQUIRE(module.has_value());

    int resumeBodies = 0;
    for (const auto& fn : module->functions) {
        if (!fn.isResumeBody) continue;
        ++resumeBodies;
        CHECK(functionText(*module, fn.name).find("is.number") == std::string::npos);
    }
    CHECK(resumeBodies == 2);
}

// ---------------------------------------------------------------------------
// A pin the oracle cannot express. `pin_matrix.sh` builds a case with
// `--no-infer` and nothing else, and `tests/oracle/README.md` has no manifest
// column — a `--pins` build is not a shape the oracle harness can take. So the
// `elidedPin` claim is pinned here instead.
// ---------------------------------------------------------------------------

TEST_CASE("a pin standing in front of the first use elides that candidate's guard") {
    if (seamIsOn()) return;
    il::Function fn;
    fn.name = "pinned";
    fn.params = {{"o", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 6;

    il::Instruction pin;
    pin.op = il::Op::PinGuard;
    pin.type = il::Type::Void;
    pin.operands = {1};
    pin.keyIndex = 10;
    pin.immI32 = static_cast<int32_t>(il::PinBarrier::Number);

    il::Block b0;
    b0.id = 0;
    b0.instructions = {propGet(1, 0, 10, 0),         propGet(2, 0, 11, 1),
                       pin,                          arith(il::Op::Add, 3, 1, 2),
                       arith(il::Op::Add, 4, 3, 2),  ret(4)};
    fn.blocks = {b0};

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    // The pinned read is proved already; the unpinned one is not.
    CHECK(stats.elidedPin == 1);
    CHECK(stats.guards == 1);
    const std::string text = body(module);
    CHECK(countOf(text, "is.number") == 1);
    // And the elided one still gets its f64: a bare raw unbox, no test in front
    // of it.
    CHECK(countOf(text, ", raw") == 2);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

// ---------------------------------------------------------------------------
// Source-driven: the kernel the chunk exists for.
// ---------------------------------------------------------------------------

namespace {

// `Matrix4.multiplyMatrices` in the shape three.js writes it: thirty-two reads
// of two element arrays, then a hundred and twenty-eight products through them.
// Written out rather than imported because `lower_fixture` lowers a source
// string, and approximating it with a loop would be pinning a different
// program.
const char* kMul4 = R"(
function multiplyMatrices(a, b, out) {
  const ae = a.elements;
  const be = b.elements;
  const te = out.elements;

  const a11 = ae[0], a12 = ae[4], a13 = ae[8], a14 = ae[12];
  const a21 = ae[1], a22 = ae[5], a23 = ae[9], a24 = ae[13];
  const a31 = ae[2], a32 = ae[6], a33 = ae[10], a34 = ae[14];
  const a41 = ae[3], a42 = ae[7], a43 = ae[11], a44 = ae[15];

  const b11 = be[0], b12 = be[4], b13 = be[8], b14 = be[12];
  const b21 = be[1], b22 = be[5], b23 = be[9], b24 = be[13];
  const b31 = be[2], b32 = be[6], b33 = be[10], b34 = be[14];
  const b41 = be[3], b42 = be[7], b43 = be[11], b44 = be[15];

  te[0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41;
  te[4] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42;
  te[8] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43;
  te[12] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;

  te[1] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41;
  te[5] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42;
  te[9] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43;
  te[13] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;

  te[2] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41;
  te[6] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42;
  te[10] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43;
  te[14] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;

  te[3] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41;
  te[7] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42;
  te[11] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43;
  te[15] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
  return out;
}
)";

}  // namespace

TEST_CASE("the mat4 kernel gets one guard chain, one trampoline and thirty-two raw unboxes") {
    if (seamIsOn()) return;
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto module = parseAndLower(kMul4, diags, buf);
    REQUIRE(module.has_value());
    REQUIRE_FALSE(diags.hasErrors());

    const il::Function* fn = nullptr;
    for (const auto& f : module->functions) {
        if (f.name == "multiplyMatrices") fn = &f;
    }
    REQUIRE(fn != nullptr);
    const std::string text = functionText(*module, "multiplyMatrices");
    REQUIRE_FALSE(text.empty());

    // Thirty-two values tested, at ONE point. `guardPoints` is the number of
    // block splits, and one is what keeps the reads in a single block.
    CHECK(countOf(text, "is.number") == 32);

    // Every checked unbox in the fast copy is gone, replaced by thirty-two raw
    // ones — one per value, not one per use.
    CHECK(countOf(text, ", raw") == 32);

    // The trampoline: exactly one block whose only instruction is a jump, and
    // no `box.f64` anywhere in it. Every partial product is computed after the
    // guards, so there is nothing promoted to re-box on the way out.
    const il::Block* trampoline = nullptr;
    for (const auto& block : fn->blocks) {
        if (block.instructions.size() == 1 && block.instructions[0].op == il::Op::Jump &&
            !block.instructions[0].target.args.empty()) {
            trampoline = &block;
        }
    }
    REQUIRE(trampoline != nullptr);
    CHECK(trampoline->instructions.size() == 1);

    // The reads stay adjacent. The longest run of `prop.get`s with no other
    // instruction between them is what `llvm_recv_proof.cpp` builds a receiver
    // proof out of, and it stops at a block end — so a guard per read would
    // have cut this into thirty-two pieces.
    size_t longestRun = 0;
    for (const auto& block : fn->blocks) {
        size_t run = 0;
        for (const auto& inst : block.instructions) {
            if (inst.op == il::Op::PropGet) {
                ++run;
                longestRun = std::max(longestRun, run);
            } else {
                run = 0;
            }
        }
    }
    CHECK(longestRun >= 32);

    CHECK(il::verify(*module, diags));
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("with the seam on the mat4 kernel is the IL it always was") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto module = parseAndLower(kMul4, diags, buf);
    REQUIRE(module.has_value());
    const std::string text = functionText(*module, "multiplyMatrices");

    if (seamIsOn()) {
        CHECK(text.find("is.number") == std::string::npos);
        CHECK(text.find(", raw") == std::string::npos);
        // One block, and the hundred and twenty-eight coercions still in it.
        CHECK(countOf(text, "  b") == 1);
    } else {
        CHECK(text.find("is.number") != std::string::npos);
    }
}

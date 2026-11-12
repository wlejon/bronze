// GUARDED NUMERIC REGIONS: WHERE A GUARD GOES (src/lower/guard_region.h).
//
// Its siblings are about which region is taken (`guard_region_test.cpp`) and
// which candidate kinds a whole-function region finds (`guard_region_entry_test.cpp`).
// This one is about the single question those two leave open: a value defined
// in one block and consumed only in others. The answer is a point at the END of
// the defining block, and the two things it has to keep true are that the point
// still reaches every use and that the slow copy can still be entered at it.
// The last case is the other placement rule, "GUARD AT DEFINITION".

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "il/verifier.h"
#include "lower/guard_region.h"
#include "lower_fixture.h"

using namespace bronze;
using namespace bronze::lower;

namespace {

il::Module oneFunction(il::Function fn) {
    il::Module module;
    module.name = "test";
    module.icSiteCount = 64;
    module.functions.push_back(std::move(fn));
    return module;
}

il::Instruction arith(il::Op op, il::ValueId result, il::ValueId a, il::ValueId b) {
    il::Instruction inst;
    inst.op = op;
    inst.type = il::Type::Dynamic;
    inst.result = result;
    inst.operands = {a, b};
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

il::Block block(il::BlockId id, std::vector<il::Instruction> insts,
                std::vector<il::BlockParam> params = {}) {
    il::Block b;
    b.id = id;
    b.params = std::move(params);
    b.instructions = std::move(insts);
    return b;
}

std::string body(const il::Module& module) {
    const std::string text = il::print(module);
    const size_t at = text.find("func ");
    return at == std::string::npos ? text : text.substr(at);
}

size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// The placement itself.
// ---------------------------------------------------------------------------

TEST_CASE("a value used only in blocks its definition dominates is guarded at the block's end") {
    // `const a = o.k; return c ? a * a : a + a;` — the read is in the header and
    // every use of it is in a `switch`/`if` arm below. There is no promoted use
    // in the header at all, so the guard cannot go in front of one; it goes in
    // front of the TERMINATOR, which is the latest point in the header, and
    // reaches both arms because a fast block's parts are a chain nothing
    // re-enters.
    il::Function fn;
    fn.name = "f";
    fn.params = {{"o", il::Type::Dynamic}, {"c", il::Type::Bool}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 5;
    fn.blocks = {
        block(0, {propGet(2, 0, 10, 0), branch(1, 1, {}, 2, {})}),
        block(1, {arith(il::Op::Mul, 3, 2, 2), ret(3)}),
        block(2, {arith(il::Op::Add, 4, 2, 2), ret(4)}),
    };

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.entryRegions == 1);
    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 1);
    CHECK(stats.guardPoints == 1);
    CHECK(stats.refusedPlacement == 0);
    CHECK(stats.refusedEntrySplit == 0);

    CHECK(body(module) ==
          "func f(%0: dynamic, %1: bool) -> dynamic {\n"
          "  b0:\n"
          "    jump b4\n"
          "  b1(%9: dynamic) [slow 0]:\n"
          "    br %1, b2, b3\n"
          "  b2 [slow 0]:\n"
          "    %3: dynamic = mul %9, %9\n"
          "    ret %3\n"
          "  b3 [slow 0]:\n"
          "    %4: dynamic = add %9, %9\n"
          "    ret %4\n"
          "  b4 [fast 0]:\n"
          "    %5: dynamic = prop.get %0, 10, 0\n"
          "    %10: bool = is.number %5\n"
          "    br %10, b5, b8\n"
          "  b5 [fast 0]:\n"
          "    %8: f64 = unbox.f64 %5, raw\n"
          "    br %1, b6, b7\n"
          "  b6 [fast 0]:\n"
          "    %6: f64 = mul %8, %8\n"
          "    %11: dynamic = box.f64 %6\n"
          "    ret %11\n"
          "  b7 [fast 0]:\n"
          "    %7: f64 = add %8, %8\n"
          "    %12: dynamic = box.f64 %7\n"
          "    ret %12\n"
          "  b8 [fast 0]:\n"
          "    jump b1(%5)\n"
          "}\n");

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("an end-of-block candidate defined before the block's first use joins that point") {
    // Two candidates in the header. `%4` has a use there, so its point is in
    // front of that use; `%3` has none, so its point would be the end of the
    // block. It does not get one: it is already defined at the earlier point,
    // and guarding a value earlier than its own first use costs nothing —
    // `is.number` reads bits. One split, one chain, one trampoline.
    il::Function fn;
    fn.name = "g";
    fn.params = {{"o", il::Type::Dynamic}, {"c", il::Type::Bool}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 8;
    fn.blocks = {
        block(0, {propGet(2, 0, 10, 0), propGet(3, 0, 11, 1), arith(il::Op::Mul, 4, 3, 3),
                  branch(1, 1, {}, 2, {})}),
        block(1, {arith(il::Op::Mul, 5, 2, 4), ret(5)}),
        block(2, {arith(il::Op::Add, 6, 2, 4), ret(6)}),
    };

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 2);
    CHECK(stats.guardPoints == 1);

    const std::string text = body(module);
    // One trampoline, because there is one point.
    CHECK(countOf(text, "is.number") == 2);
    CHECK(countOf(text, ", raw") == 2);
}

TEST_CASE("an end-of-block candidate defined after that point takes a second split") {
    // The same two candidates in the other order: the read whose uses are all
    // below now comes AFTER the header's own promoted use, so it does not exist
    // at that point and cannot be tested there. Two points, and that is forced
    // rather than chosen — a guard before its value's definition has nothing to
    // read.
    il::Function fn;
    fn.name = "g2";
    fn.params = {{"o", il::Type::Dynamic}, {"c", il::Type::Bool}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 8;
    fn.blocks = {
        block(0, {propGet(2, 0, 10, 0), arith(il::Op::Mul, 3, 2, 2), propGet(4, 0, 11, 1),
                  branch(1, 1, {}, 2, {})}),
        block(1, {arith(il::Op::Mul, 5, 4, 3), ret(5)}),
        block(2, {arith(il::Op::Add, 6, 4, 3), ret(6)}),
    };

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 2);
    CHECK(stats.guardPoints == 2);
}

TEST_CASE("a promoted use the definition does not dominate is still refused") {
    // `b3` has no predecessor. An entry region is EVERY block of the function,
    // reachable or not, and dominance is not defined over a block the entry
    // cannot reach — so there is no point in `b0` that reaches `b3`'s use of
    // `%2`, and the region is declined whole rather than half-guarded. It is
    // what `Matrix4.makeOrthographic` is: its `else` arm throws, and the block
    // the arm would have fallen into is unreachable.
    il::Function fn;
    fn.name = "h";
    fn.params = {{"o", il::Type::Dynamic}, {"c", il::Type::Bool}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 5;
    fn.blocks = {
        block(0, {propGet(2, 0, 10, 0), branch(1, 1, {}, 2, {})}),
        block(1, {arith(il::Op::Mul, 3, 2, 2), ret(3)}),
        block(2, {ret(0)}),
        block(3, {arith(il::Op::Mul, 4, 2, 2), ret(4)}),
    };

    const il::Module before = oneFunction(fn);
    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    CHECK_FALSE(applyGuardedRegions(module, &stats));

    CHECK(stats.refusedPlacement == 1);
    CHECK(stats.refusedEntrySplit == 0);
    CHECK(stats.duplicated == 0);
    // Refused means UNTOUCHED, not partly rewritten.
    CHECK(il::print(module) == il::print(before));
}

// ---------------------------------------------------------------------------
// The entry region's split block.
// ---------------------------------------------------------------------------

TEST_CASE("an entry region splits below its header when the split block dominates what it reaches") {
    // The shape a DEFAULTED PARAMETER makes, which is `Quaternion.setFromEuler`:
    // the header only chooses whether the default runs, and the body — the read
    // whose uses are all in the arms below — lives in the join. The join is not
    // the header, and it dominates every block reachable from it, which is the
    // condition the orphaned prefix's rename needs.
    il::Function fn;
    fn.name = "k";
    fn.params = {{"o", il::Type::Dynamic}, {"c", il::Type::Bool}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 7;
    fn.blocks = {
        block(0, {branch(1, 1, {}, 2, {0})}),
        block(1, {propGet(2, 0, 10, 0), jump(2, {2})}),
        block(2, {propGet(4, 3, 11, 1), branch(1, 3, {}, 4, {})}, {{3, il::Type::Dynamic}}),
        block(3, {arith(il::Op::Mul, 5, 4, 4), ret(5)}),
        block(4, {arith(il::Op::Add, 6, 4, 4), ret(6)}),
    };

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.entryRegions == 1);
    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 1);
    CHECK(stats.refusedEntrySplit == 0);

    const std::string text = body(module);
    CHECK(countOf(text, "is.number") == 1);
    CHECK(countOf(text, ", raw") == 1);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("an entry region re-derives the names below a split block that does not dominate what it reaches") {
    // `b1` holds the only split and does not dominate `b3`, which it reaches.
    // `b3` reads `%2`, defined in the header — and with no split in the header
    // the header's slow copy is orphaned and pruned, while `b3`'s survives on
    // the trampoline's edge. The dominator chain of `b3` does not hold `b1`, so
    // the names are re-derived over the pruned rewrite (guard_region_ssa.cpp):
    // `b3`'s only surviving predecessor is the tail, and it reads the tail's
    // parameter.
    //
    // `%5` is dead: two promotable operations are an entry region's floor
    // (`kMinEntryUses`) and this is the smallest body that reaches it.
    il::Function fn;
    fn.name = "m";
    fn.params = {{"o", il::Type::Dynamic}, {"c", il::Type::Bool}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 7;
    fn.blocks = {
        block(0, {propGet(2, 0, 10, 0), branch(1, 1, {}, 2, {})}),
        block(1, {propGet(3, 2, 11, 1), arith(il::Op::Mul, 4, 3, 3),
                  arith(il::Op::Mul, 5, 4, 3), jump(3)}),
        block(2, {jump(3)}),
        block(3, {propGet(6, 2, 12, 2), ret(6)}),
    };

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.refusedEntrySplit == 0);
    CHECK(stats.refusedSsa == 0);
    CHECK(stats.repairedNames == 1);
    CHECK(stats.duplicated == 1);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("a join the slow copy reaches from a tail and from its head takes a parameter") {
    // The same shape with a guarded PARAMETER `%2`: a failed entry guard enters
    // the slow header, so the slow copy of `b0` survives the pruning and `b3`
    // is reached both from the tail of `b1` (which carries `%3` as a parameter)
    // and from `b2` (which carries the header's own `%3`). Neither dominates
    // `b3`, so `b3` takes a parameter for it, fed by both.
    il::Function fn;
    fn.name = "j";
    fn.params = {{"o", il::Type::Dynamic}, {"c", il::Type::Bool}, {"x", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 10;
    fn.blocks = {
        block(0, {propGet(3, 0, 10, 0), arith(il::Op::Mul, 4, 2, 2), arith(il::Op::Mul, 5, 4, 2),
                  branch(1, 1, {}, 2, {})}),
        block(1, {propGet(6, 3, 11, 1), arith(il::Op::Mul, 7, 6, 6), arith(il::Op::Mul, 8, 7, 6),
                  jump(3)}),
        block(2, {jump(3)}),
        block(3, {propGet(9, 3, 12, 2), ret(9)}),
    };

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.repairedNames == 1);
    CHECK(stats.duplicated == 1);
    CHECK(stats.refusedSsa == 0);

    // The slow join reads `%3` through a parameter of its own.
    bool joinParam = false;
    for (const il::Block& b : module.functions[0].blocks) {
        if (b.copyClass != il::CopyClass::Slow || b.params.empty()) continue;
        for (const il::Instruction& inst : b.instructions) {
            if (inst.op == il::Op::PropGet && inst.keyIndex == 12 &&
                inst.operands[0] == b.params.back().id) {
                joinParam = true;
            }
        }
    }
    CHECK(joinParam);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
    CHECK_FALSE(diags.hasErrors());
}

// ---------------------------------------------------------------------------
// GUARD AT DEFINITION: a test stands where its value's read stretch ends.
// ---------------------------------------------------------------------------

TEST_CASE("a read is tested where its receiver's reads end, and leaves a later exit rebuilt from its double") {
    // `const a = o.x, b = o.y, c = p.z; return a * b + c + a;` — `a`'s first
    // use is past the read off `p`, whose miss is a call. Its test does not
    // wait for that use: it stands with `b`'s behind the two reads off `o`, in
    // front of the read off `p`, so neither box is live across that read. `c`
    // is tested at its own first use, and the trampoline there passes `a`,
    // which the slow copy still adds, as a raw box of its double and the
    // untested `c` as the box it is.
    il::Function fn;
    fn.name = "h";
    fn.params = {{"o", il::Type::Dynamic}, {"p", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 8;
    fn.blocks = {
        block(0, {propGet(2, 0, 10, 0), propGet(3, 0, 11, 1), propGet(4, 1, 12, 2), arith(il::Op::Mul, 5, 2, 3),
                  arith(il::Op::Add, 6, 5, 4), arith(il::Op::Add, 7, 6, 2), ret(7)}),
    };

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 3);
    CHECK(stats.guardPoints == 2);

    CHECK(body(module) ==
          "func h(%0: dynamic, %1: dynamic) -> dynamic {\n"
          "  b0:\n"
          "    jump b3\n"
          "  b1(%17: dynamic, %18: dynamic) [slow 0]:\n"
          "    %4: dynamic = prop.get %1, 12, 2\n"
          "    %5: dynamic = mul %17, %18\n"
          "    jump b2(%17, %4, %5)\n"
          "  b2(%19: dynamic, %20: dynamic, %21: dynamic) [slow 0]:\n"
          "    %6: dynamic = add %21, %20\n"
          "    %7: dynamic = add %6, %19\n"
          "    ret %7\n"
          "  b3 [fast 0]:\n"
          "    %8: dynamic = prop.get %0, 10, 0\n"
          "    %9: dynamic = prop.get %0, 11, 1\n"
          "    %22: bool = is.number %8\n"
          "    br %22, b4, b7\n"
          "  b4 [fast 0]:\n"
          "    %23: bool = is.number %9\n"
          "    br %23, b5, b7\n"
          "  b5 [fast 0]:\n"
          "    %14: f64 = unbox.f64 %8, raw\n"
          "    %15: f64 = unbox.f64 %9, raw\n"
          "    %10: dynamic = prop.get %1, 12, 2\n"
          "    %11: f64 = mul %14, %15\n"
          "    %24: bool = is.number %10\n"
          "    br %24, b6, b8\n"
          "  b6 [fast 0]:\n"
          "    %16: f64 = unbox.f64 %10, raw\n"
          "    %12: f64 = add %11, %16\n"
          "    %13: f64 = add %12, %14\n"
          "    %27: dynamic = box.f64 %13\n"
          "    ret %27\n"
          "  b7 [fast 0]:\n"
          "    jump b1(%8, %9)\n"
          "  b8 [fast 0]:\n"
          "    %25: dynamic = box.f64 %14, raw\n"
          "    %26: dynamic = box.f64 %11\n"
          "    jump b2(%25, %10, %26)\n"
          "}\n");

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
    CHECK_FALSE(diags.hasErrors());
}

// GUARDED NUMERIC REGIONS: WHICH LOOP GETS THE FUNCTION'S BUDGET
// (src/lower/guard_region_apply.cpp, `fallbackOfTypedCopies`).
//
// A function's regions share one growth budget. typed_loop_version.cpp leaves
// a versioned loop's original in place, in front of its typed copies in block
// order and entered only on the failing edge of their last entry test. Taken
// in block order the budget went to that original, and the copy that runs in
// its place had no region.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "il/il.h"
#include "il/verifier.h"
#include "lower/guard_region.h"
#include "support/diagnostics.h"

using namespace bronze;
using namespace bronze::lower;

namespace {

il::Instruction make(il::Op op, il::Type type, il::ValueId result, std::vector<il::ValueId> operands) {
    il::Instruction inst;
    inst.op = op;
    inst.type = type;
    inst.result = result;
    inst.operands = std::move(operands);
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
    inst.target = {thenBlock, std::move(thenArgs)};
    inst.elseTarget = {elseBlock, std::move(elseArgs)};
    return inst;
}

constexpr int kReads = 16;

// `for (let i = 0; i < n; i++) t = t + o.a + o.a + ...` (kReads reads) as
// three blocks from `header`, leaving for `exit` with the total. `next` is the
// first free value id and is advanced past the loop's.
void addLoop(il::Function& fn, il::BlockId header, il::BlockId exit, il::ValueId& next) {
    const il::ValueId total = next++, i = next++, test = next++;
    il::Block head;
    head.id = header;
    head.params = {{total, il::Type::Dynamic}, {i, il::Type::F64}};
    head.instructions = {make(il::Op::CmpLt, il::Type::Bool, test, {i, 1}),
                         branch(test, header + 1, {}, exit, {total})};

    il::Block body;
    body.id = header + 1;
    il::ValueId sum = total;
    for (int r = 0; r < kReads; ++r) {
        il::Instruction read = make(il::Op::PropGet, il::Type::Dynamic, next++, {0});
        read.keyIndex = 10;
        read.icIndex = static_cast<uint32_t>(r);
        const il::ValueId added = next++;
        body.instructions.push_back(read);
        body.instructions.push_back(make(il::Op::Add, il::Type::Dynamic, added, {sum, read.result}));
        sum = added;
    }
    body.instructions.push_back(jump(header + 2, {sum, i}));

    const il::ValueId carried = next++, at = next++, one = next++, stepped = next++;
    il::Block latch;
    latch.id = header + 2;
    latch.params = {{carried, il::Type::Dynamic}, {at, il::Type::F64}};
    il::Instruction c = make(il::Op::ConstF64, il::Type::F64, one, {});
    c.immF64 = 1;
    latch.instructions = {c, make(il::Op::Add, il::Type::F64, stepped, {at, one}), jump(header, {carried, stepped})};

    fn.blocks.push_back(std::move(head));
    fn.blocks.push_back(std::move(body));
    fn.blocks.push_back(std::move(latch));
}

// Two copies of one loop behind an entry test: the test's passing edge enters
// the second, its failing edge the first. With `typed` the test is `is.array`
// of the receiver, which is how a versioned loop's original and its Array copy
// stand; without, a compare of the count.
il::Module twoLoops(bool typed) {
    il::Function fn;
    fn.name = "loops";
    fn.params = {{"o", il::Type::Dynamic}, {"n", il::Type::F64}};
    fn.returnType = il::Type::Dynamic;
    il::ValueId next = 2;
    const il::ValueId zero = next++, start = next++, boxed = next++, cond = next++;

    il::Block entry;
    entry.id = 0;
    il::Instruction z = make(il::Op::ConstF64, il::Type::F64, zero, {});
    il::Instruction s = make(il::Op::ConstF64, il::Type::F64, start, {});
    il::Instruction b = make(il::Op::Box, il::Type::Dynamic, boxed, {zero});
    b.boxType = il::Type::F64;
    entry.instructions = {z, s, b,
                          typed ? make(il::Op::IsArray, il::Type::Bool, cond, {0})
                                : make(il::Op::CmpLt, il::Type::Bool, cond, {zero, 1}),
                          branch(cond, 4, {boxed, start}, 1, {boxed, start})};
    fn.blocks.push_back(std::move(entry));

    addLoop(fn, 1, 7, next);
    addLoop(fn, 4, 7, next);

    const il::ValueId result = next++;
    il::Block exit;
    exit.id = 7;
    exit.params = {{result, il::Type::Dynamic}};
    il::Instruction r;
    r.op = il::Op::Ret;
    r.operands = {result};
    exit.instructions = {r};
    fn.blocks.push_back(std::move(exit));
    fn.valueCount = next;

    il::Module module;
    module.name = "test";
    module.icSiteCount = kReads;
    module.functions.push_back(std::move(fn));
    return module;
}

}  // namespace

TEST_CASE("the budget goes to the typed copy, not to the loop its entry test falls back to") {
    il::Module module = twoLoops(/*typed=*/true);
    DiagnosticSink before;
    REQUIRE(il::verify(module, before));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));
    DiagnosticSink after;
    REQUIRE(il::verify(module, after));

    // Room for one copy: the other loop is as large again.
    CHECK(stats.duplicated == 1);
    const il::Function& fn = module.functions[0];
    CHECK(fn.blocks[1].copyClass == il::CopyClass::Shared);
    CHECK(fn.blocks[4].copyClass == il::CopyClass::Slow);
}

TEST_CASE("two loops behind an ordinary branch are taken in block order") {
    il::Module module = twoLoops(/*typed=*/false);
    DiagnosticSink before;
    REQUIRE(il::verify(module, before));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));
    DiagnosticSink after;
    REQUIRE(il::verify(module, after));

    CHECK(stats.duplicated == 1);
    const il::Function& fn = module.functions[0];
    CHECK(fn.blocks[1].copyClass == il::CopyClass::Slow);
}

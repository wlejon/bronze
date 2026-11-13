// GUARDED REGIONS: IC EXITS (src/lower/guard_region.h, "IC EXITS").
//
// An inlined accessor or method body stands behind its site's guard, and the
// guard's miss runs the original access and rejoins. In the fast copy the miss
// leaves for the slow copy instead, in front of the slow copy's own guard. The
// cases here are which guards become exits and what a loop needs to be taken
// for them alone.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "il/print.h"
#include "il/verifier.h"
#include "lower/guard_region.h"

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

il::Instruction constF64(il::ValueId result, double value) {
    il::Instruction inst = make(il::Op::ConstF64, il::Type::F64, result, {});
    inst.immF64 = value;
    return inst;
}

// The guard of a body inlined at the accessor site whose cell pair is `cell`.
il::Instruction guard(il::ValueId result, il::ValueId recv, uint32_t cell, uint32_t callee) {
    il::Instruction inst = make(il::Op::AccessorIcIs, il::Type::Bool, result, {recv});
    inst.staticCellIndex = cell;
    inst.calleeIndex = callee;
    return inst;
}

// The original site: `recv.<key>` through the accessor `callee`.
il::Instruction accessorGet(il::ValueId result, il::ValueId recv, uint32_t key, uint32_t cell, uint32_t callee) {
    il::Instruction inst = make(il::Op::PropGet, il::Type::Dynamic, result, {recv});
    inst.keyIndex = key;
    inst.icIndex = cell;
    inst.staticCellIndex = cell;
    inst.directTarget = callee;
    return inst;
}

// What an inlined getter body is here: a read of the backing field.
il::Instruction fieldGet(il::ValueId result, il::ValueId recv, uint32_t key, uint32_t ic) {
    il::Instruction inst = make(il::Op::PropGet, il::Type::Dynamic, result, {recv});
    inst.keyIndex = key;
    inst.icIndex = ic;
    return inst;
}

il::Instruction jump(il::BlockId target, std::vector<il::ValueId> args = {}) {
    il::Instruction inst = make(il::Op::Jump, il::Type::Void, il::kNoValue, {});
    inst.target.block = target;
    inst.target.args = std::move(args);
    return inst;
}

il::Instruction branch(il::ValueId cond, il::BlockId thenBlock, il::BlockId elseBlock) {
    il::Instruction inst = make(il::Op::Branch, il::Type::Void, il::kNoValue, {cond});
    inst.target.block = thenBlock;
    inst.elseTarget.block = elseBlock;
    return inst;
}

il::Block block(il::BlockId id, std::vector<il::Instruction> insts, std::vector<il::BlockParam> params = {}) {
    il::Block b;
    b.id = id;
    b.params = std::move(params);
    b.instructions = std::move(insts);
    return b;
}

// A getter the sites name: it only has to exist.
il::Function getter(const char* name) {
    il::Function fn;
    fn.name = name;
    fn.params = {{"this", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 1;
    fn.blocks = {block(0, {make(il::Op::Ret, il::Type::Void, il::kNoValue, {0})})};
    return fn;
}

il::Module moduleOf(il::Function fn) {
    il::Module module;
    module.name = "test";
    module.icSiteCount = 64;
    module.staticSiteCount = 64;
    module.keyConstants = {"x", "y", "_x", "_y"};
    module.functions.push_back(std::move(fn));
    module.functions.push_back(getter("get x"));
    module.functions.push_back(getter("get y"));
    return module;
}

// The head of `for (i = 0; i < n; i++)` over receiver %0 and bound %1: block 0
// enters the header, block 1, whose body starts at block 2 and whose exit is
// `exit`. The counter is %3 and the latch `latch` steps it.
void loopHead(il::Function& fn, il::BlockId exit) {
    fn.params = {{"o", il::Type::Dynamic}, {"n", il::Type::F64}};
    fn.returnType = il::Type::Dynamic;
    fn.blocks.push_back(block(0, {constF64(2, 0), jump(1, {2})}));
    fn.blocks.push_back(
        block(1, {make(il::Op::CmpLt, il::Type::Bool, 4, {3, 1}), branch(4, 2, exit)}, {{3, il::Type::F64}}));
}
il::Block latch(il::BlockId id, il::ValueId one, il::ValueId next) {
    return block(id, {constF64(one, 1), make(il::Op::Add, il::Type::F64, next, {3, one}), jump(1, {next})});
}
il::Block exitBlock(il::BlockId id) { return block(id, {make(il::Op::Ret, il::Type::Void, il::kNoValue, {0})}); }

// `o.x; o.y` in a loop, both getters inlined: two diamonds, one after the
// other, and no arithmetic.
il::Function twoDiamonds() {
    il::Function fn;
    fn.name = "two";
    loopHead(fn, 9);
    fn.blocks.push_back(block(2, {guard(5, 0, 10, 1), branch(5, 3, 4)}));
    fn.blocks.push_back(block(3, {fieldGet(6, 0, 2, 20), jump(5, {6})}));
    fn.blocks.push_back(block(4, {accessorGet(7, 0, 0, 10, 1), jump(5, {7})}));
    fn.blocks.push_back(block(5, {guard(9, 0, 12, 2), branch(9, 6, 7)}, {{8, il::Type::Dynamic}}));
    fn.blocks.push_back(block(6, {fieldGet(10, 0, 3, 21), jump(8, {10})}));
    fn.blocks.push_back(block(7, {accessorGet(11, 0, 1, 12, 2), jump(8, {11})}));
    fn.blocks.push_back(latch(8, 13, 14));
    fn.blocks[8].params = {{12, il::Type::Dynamic}};
    fn.blocks.push_back(exitBlock(9));
    fn.valueCount = 15;
    return fn;
}

std::string body(const il::Module& module) {
    const std::string text = il::print(module);
    const size_t at = text.find("func two");
    const size_t end = text.find("\n}", at);
    return at == std::string::npos ? text : text.substr(at, end - at);
}

size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// The original sites left in blocks of copy class `cls`.
size_t sitesIn(const il::Function& fn, il::CopyClass cls) {
    size_t n = 0;
    for (const il::Block& b : fn.blocks) {
        if (b.copyClass != cls) continue;
        for (const il::Instruction& inst : b.instructions) {
            if (inst.op == il::Op::PropGet && inst.directTarget != il::Instruction::kNoDirectTarget) ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("a loop of two inlined getters is taken for its exits: the fast copy has no miss arm") {
    il::Module module = moduleOf(twoDiamonds());
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.icExits == 2);
    CHECK(stats.guardPoints == 2);
    CHECK(stats.guards == 0);

    // Each guard is in both copies; each original site only in the slow one,
    // behind the slow copy's own guard.
    const il::Function& fn = module.functions[0];
    CHECK(countOf(body(module), "accessor.ic.is") == 4);
    CHECK(sitesIn(fn, il::CopyClass::Fast) == 0);
    CHECK(sitesIn(fn, il::CopyClass::Slow) == 2);
    // A fast guard's false edge is a trampoline, which only jumps.
    for (const il::Block& b : fn.blocks) {
        if (b.copyClass != il::CopyClass::Fast || b.instructions.size() < 2) continue;
        const il::Instruction& g = b.instructions[b.instructions.size() - 2];
        if (g.op != il::Op::AccessorIcIs) continue;
        const il::Block& miss = fn.blocks[b.instructions.back().elseTarget.block];
        CHECK(miss.copyClass == il::CopyClass::Fast);
        REQUIRE(miss.instructions.size() == 1);
        CHECK(miss.instructions[0].op == il::Op::Jump);
        // It lands in front of the slow copy's guard.
        const il::Block& tail = fn.blocks[miss.instructions[0].target.block];
        CHECK(tail.copyClass == il::CopyClass::Slow);
        CHECK(tail.instructions.front().op == il::Op::AccessorIcIs);
    }

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("one exit alone does not take a loop") {
    il::Function fn;
    fn.name = "two";
    loopHead(fn, 6);
    fn.blocks.push_back(block(2, {guard(5, 0, 10, 1), branch(5, 3, 4)}));
    fn.blocks.push_back(block(3, {fieldGet(6, 0, 2, 20), jump(5, {6})}));
    fn.blocks.push_back(block(4, {accessorGet(7, 0, 0, 10, 1), jump(5, {7})}));
    fn.blocks.push_back(latch(5, 9, 10));
    fn.blocks[5].params = {{8, il::Type::Dynamic}};
    fn.blocks.push_back(exitBlock(6));
    fn.valueCount = 11;
    il::Module module = moduleOf(std::move(fn));

    GuardRegionStats stats;
    CHECK_FALSE(applyGuardedRegions(module, &stats));
    CHECK(stats.regions == 1);
    CHECK(stats.duplicated == 0);
    CHECK(stats.icExits == 0);
}

TEST_CASE("a guard that misses into another guard is no exit: only the one whose miss is the site") {
    // The first diamond's miss tries a second guard on the same cell before
    // the site, as a site with two guessed bodies does.
    il::Function fn;
    fn.name = "two";
    loopHead(fn, 11);
    fn.blocks.push_back(block(2, {guard(5, 0, 10, 1), branch(5, 3, 4)}));
    fn.blocks.push_back(block(3, {fieldGet(6, 0, 2, 20), jump(7, {6})}));
    fn.blocks.push_back(block(4, {guard(15, 0, 10, 2), branch(15, 5, 6)}));
    fn.blocks.push_back(block(5, {fieldGet(16, 0, 3, 22), jump(7, {16})}));
    fn.blocks.push_back(block(6, {accessorGet(7, 0, 0, 10, 1), jump(7, {7})}));
    fn.blocks.push_back(block(7, {guard(9, 0, 12, 2), branch(9, 8, 9)}, {{8, il::Type::Dynamic}}));
    fn.blocks.push_back(block(8, {fieldGet(10, 0, 3, 21), jump(10, {10})}));
    fn.blocks.push_back(block(9, {accessorGet(11, 0, 1, 12, 2), jump(10, {11})}));
    fn.blocks.push_back(latch(10, 13, 14));
    fn.blocks[10].params = {{12, il::Type::Dynamic}};
    fn.blocks.push_back(exitBlock(11));
    fn.valueCount = 17;
    il::Module module = moduleOf(std::move(fn));

    // The second guard's block does not dominate the join both bodies reach,
    // so its split is one the run after inlining serves: that run repairs the
    // slow copy's names, and it is the run that sees inlined bodies at all.
    GuardRegionStats stats;
    const std::vector<uint32_t> only = {0};
    REQUIRE(applyGuardedRegions(module, &stats, nullptr, &only));
    CHECK(stats.duplicated == 1);
    CHECK(stats.icExits == 2);
    // Three guards in the fast copy, and the first still has both its arms
    // there: its miss is the second guard's block, not a trampoline. The slow
    // copy keeps the two a trampoline lands in front of; its own first guard
    // went with its header, which nothing enters once the back edge returns
    // to the fast copy and no test stands on the way.
    const il::Function& out = module.functions[0];
    CHECK(countOf(body(module), "accessor.ic.is") == 5);
    CHECK(sitesIn(out, il::CopyClass::Fast) == 0);
    size_t twoWay = 0;
    for (const il::Block& b : out.blocks) {
        if (b.copyClass != il::CopyClass::Fast || b.instructions.size() < 2) continue;
        if (b.instructions[b.instructions.size() - 2].op != il::Op::AccessorIcIs) continue;
        const il::Block& miss = out.blocks[b.instructions.back().elseTarget.block];
        if (miss.instructions.size() == 2 && miss.instructions[0].op == il::Op::AccessorIcIs) ++twoWay;
    }
    CHECK(twoWay == 1);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("the run after inlining comes back: the slow copy's back edge reaches the fast header") {
    // guard_region.h, "RE-ENTRY". The first run leaves for the rest of the
    // loop; the run that names its functions leaves for one iteration.
    const auto wayBack = [](const il::Function& fn) {
        // Jumps from a Slow block into a Fast one. A trampoline is Fast and
        // jumps to Slow, so only the way back counts.
        size_t n = 0;
        for (const il::Block& b : fn.blocks) {
            if (b.copyClass != il::CopyClass::Slow || b.instructions.empty()) continue;
            const il::Instruction& last = b.instructions.back();
            if (last.op == il::Op::Jump && fn.blocks[last.target.block].copyClass == il::CopyClass::Fast) ++n;
        }
        return n;
    };

    il::Module first = moduleOf(twoDiamonds());
    REQUIRE(applyGuardedRegions(first));
    CHECK(wayBack(first.functions[0]) == 0);

    il::Module module = moduleOf(twoDiamonds());
    GuardRegionStats stats;
    const std::vector<uint32_t> only = {0};
    REQUIRE(applyGuardedRegions(module, &stats, nullptr, &only));
    CHECK(stats.icExits == 2);
    const il::Function& fn = module.functions[0];
    CHECK(wayBack(fn) == 1);
    // What it enters is the preheader, the fast header's one predecessor
    // outside the fast loop, and the entry edge enters it too.
    il::BlockId pre = il::kNoBlock;
    for (const il::Block& b : fn.blocks) {
        if (b.copyClass != il::CopyClass::Slow || b.instructions.empty()) continue;
        const il::Instruction& last = b.instructions.back();
        if (last.op == il::Op::Jump && fn.blocks[last.target.block].copyClass == il::CopyClass::Fast) {
            pre = last.target.block;
        }
    }
    REQUIRE(pre != il::kNoBlock);
    size_t into = 0;
    for (const il::Block& b : fn.blocks) {
        if (b.instructions.empty()) continue;
        const il::Instruction& last = b.instructions.back();
        if (last.op == il::Op::Jump && last.target.block == pre) ++into;
        if (last.op == il::Op::Branch && (last.target.block == pre || last.elseTarget.block == pre)) ++into;
    }
    CHECK(into == 2);
    CHECK(fn.blocks[pre].instructions.back().op == il::Op::Jump);
    CHECK(fn.blocks[fn.blocks[pre].instructions.back().target.block].copyClass == il::CopyClass::Fast);
    // Both original sites are still only in the slow copy.
    CHECK(sitesIn(fn, il::CopyClass::Fast) == 0);
    CHECK(sitesIn(fn, il::CopyClass::Slow) == 2);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

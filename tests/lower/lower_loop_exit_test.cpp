// What a loop's EXIT edge hands the exit block, when the condition is an
// expression that assigns.
//
// The oracle case `loop_condition_side_effects` pins the printed answer; this
// pins the IL that has to produce it, because the two failures look nothing
// alike from the outside. A loop whose exit block has the right parameters, the
// right types and the right number of edges can still be wrong in the argument
// list alone — every value named there is live and well typed, they are just
// the values from the top of the header rather than from the branch. Nothing
// but a test that reads the argument list can say which.
//
// The signature of the bug, in one sentence: the argument the exit edge passes
// for a variable the CONDITION assigned is the header's parameter for that
// variable — the value on the way in, one evaluation stale.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

namespace {

const il::Function* mainOf(const il::Module& mod) {
    for (const auto& fn : mod.functions) {
        if (fn.name == "main") return &fn;
    }
    return nullptr;
}

struct BranchSite {
    const il::Block* block = nullptr;
    const il::Instruction* inst = nullptr;
};

// The loop's test lives at the function's only branch in every program below,
// so finding it needs no knowledge of the block numbering — which is what keeps
// this test from re-breaking every time an unrelated block is added.
BranchSite onlyBranch(const il::Function& fn) {
    BranchSite found;
    size_t count = 0;
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.op != il::Op::Branch) continue;
            ++count;
            found = BranchSite{&block, &inst};
        }
    }
    REQUIRE(count == 1);
    return found;
}

// `first` is assigned by the condition out of `second`, and both are loop
// variables in declaration order, so the exiting edge owes the exit block the
// same value twice — and neither copy may be the parameter the branch's own
// block was entered with for `first`.
void checkExitEdgeIsPostCondition(const il::Module& mod) {
    const il::Function* main = mainOf(mod);
    REQUIRE(main != nullptr);
    const BranchSite site = onlyBranch(*main);
    REQUIRE(site.block->params.size() == 2);

    const std::vector<il::ValueId>& args = site.inst->elseTarget.args;
    REQUIRE(args.size() == 2);
    CHECK(args[0] == args[1]);
    // The second variable is the counter, untouched by the condition, so its
    // argument IS the parameter — which is what makes the first one's
    // difference meaningful rather than an artifact of renumbering.
    CHECK(args[1] == site.block->params[1].id);
    CHECK(args[0] != site.block->params[0].id);
}

}  // namespace

TEST_CASE("a for loop's exit edge carries what the condition assigned") {
    // 14.7.4.8 step 3.a. The test runs once more than the body does, so the
    // write it makes on that last trip is one the body never made — and the
    // exit edge is the only place it can be picked up.
    const std::string src =
        "let seen = -1;\n"
        "for (let i = 0; (seen = i), i < 3; i++) {}\n"
        "console.log(seen);\n";

    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto inferred = inferAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(inferred.has_value());
    checkExitEdgeIsPostCondition(*inferred);

    // And on the uniform dynamic convention, where every parameter is dynamic
    // and a boxing coercion could plausibly have introduced a fresh value that
    // masked the staleness.
    DiagnosticSink noInferDiags;
    SourceBuffer noInferBuf("test.ts", "");
    const auto plain = parseAndLower(src, noInferDiags, noInferBuf);
    REQUIRE_FALSE(noInferDiags.hasErrors());
    REQUIRE(plain.has_value());
    checkExitEdgeIsPostCondition(*plain);
}

TEST_CASE("a while loop's exit edge carries what the condition assigned") {
    // 14.7.2 step 2.a. `while` has always been right here; the check is a
    // ratchet on the shape the two other forms are held to.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = inferAndLower(
        "let seen = -1;\n"
        "let j = 0;\n"
        "while (((seen = j), j < 3)) { j++; }\n"
        "console.log(seen);\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    checkExitEdgeIsPostCondition(*mod);
}

TEST_CASE("a do-while's exit edge carries what the condition assigned") {
    // 14.7.3 step 2.d. The branch is in the condition block rather than the
    // header, and the same rule applies to it: the arguments belong where the
    // branch is, not where the block started.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = inferAndLower(
        "let seen = -1;\n"
        "let k = 0;\n"
        "do { k++; } while (((seen = k), k < 3));\n"
        "console.log(seen);\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    checkExitEdgeIsPostCondition(*mod);
}

TEST_CASE("a write made only on the exiting evaluation reaches the exit block") {
    // The short-circuit shape: `i < 3` is the real test and the right-hand side
    // runs on exactly one evaluation — the one that ends the loop. So the
    // assignment is not merely LATE on the exit edge, it is absent from every
    // other edge in the function, and the branch that carries it is not even in
    // the header block. Collecting the arguments in the header could not
    // express this value at all.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = inferAndLower(
        "let t = -1;\n"
        "for (let i = 0; i < 3 || ((t = 99), false); i++) {}\n"
        "console.log(t);\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    const il::Function* main = mainOf(*mod);
    REQUIRE(main != nullptr);

    // The header is whatever the entry jump out of b0 targets, and its first
    // parameter is `t` on the way in.
    REQUIRE_FALSE(main->blocks[0].instructions.empty());
    const il::Instruction& entry = main->blocks[0].instructions.back();
    REQUIRE(entry.op == il::Op::Jump);
    const il::Block& header = main->blocks[entry.target.block];
    REQUIRE(header.params.size() == 2);

    // The exit block is the one that prints, and no edge into it may pass the
    // header's `t` — the assignment happened after the header on every path
    // that reaches it.
    il::BlockId exitBlock = il::kNoBlock;
    for (const auto& block : main->blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.op == il::Op::Print) exitBlock = block.id;
        }
    }
    REQUIRE(exitBlock != il::kNoBlock);

    size_t edges = 0;
    for (const auto& block : main->blocks) {
        for (const auto& inst : block.instructions) {
            for (const il::BlockTarget* target : {&inst.target, &inst.elseTarget}) {
                if (target->block != exitBlock) continue;
                ++edges;
                REQUIRE(target->args.size() == 2);
                CHECK(target->args[0] != header.params[0].id);
            }
        }
    }
    CHECK(edges == 1);
}

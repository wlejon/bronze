// FUNCTION PLACEMENT (src/lower/function_placement.h): which functions lead a
// module's text and which close it, from a harvest's samples and caches, and
// what moves without a profile.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "lower/function_placement.h"
#include "opt/spec/feedback_plan.h"

using namespace bronze;

namespace {

il::Instruction ret() {
    il::Instruction inst;
    inst.op = il::Op::Ret;
    return inst;
}

il::Instruction call(uint32_t callee) {
    il::Instruction inst;
    inst.op = il::Op::Call;
    inst.type = il::Type::Dynamic;
    inst.result = 0;
    inst.calleeIndex = callee;
    return inst;
}

il::Instruction propGet(uint32_t ic) {
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = 1;
    inst.operands = {0};
    inst.icIndex = ic;
    return inst;
}

il::Function function(const std::string& name, std::vector<il::Instruction> body) {
    il::Function fn;
    fn.name = name;
    body.push_back(ret());
    il::Block block;
    block.instructions = std::move(body);
    fn.blocks.push_back(std::move(block));
    return fn;
}

// 0 main (top level), 1 frame (sampled, calls 2), 2 leaf (no samples; its
// cache filled), 3 setup (a sample, under the hot share), 4 never (its cache
// could have filled and did not), 5 quiet (nothing says anything).
il::Module program() {
    il::Module mod;
    mod.name = "test";
    mod.icSiteCount = 3;
    il::Function main = function("main", {});
    main.isEntryPoint = true;
    mod.functions.push_back(std::move(main));
    mod.functions.push_back(function("frame", {call(2), propGet(0)}));
    mod.functions.push_back(function("leaf", {propGet(1)}));
    mod.functions.push_back(function("setup", {}));
    mod.functions.push_back(function("never", {propGet(2)}));
    mod.functions.push_back(function("quiet", {}));
    return mod;
}

spec::FeedbackPlan harvest() {
    spec::FeedbackPlan plan;
    plan.samples["frame"] = 1000;
    plan.samples["setup"] = 1;
    plan.shapesByIc[0].total = 1;
    plan.shapesByIc[0].counts["S"] = 1;
    plan.shapesByIc[1].total = 1;
    plan.shapesByIc[1].counts["S"] = 1;
    return plan;
}

}  // namespace

TEST_CASE("placement: sampled functions lead, ran callees follow, never-ran and top level close") {
    il::Module mod = program();
    const spec::FeedbackPlan plan = harvest();
    const lower::PlacementStats stats = lower::planFunctionPlacement(mod, &plan);
    CHECK(stats.profiled);
    CHECK(mod.hotFunctions == std::vector<uint32_t>{1, 2});
    CHECK(mod.coldFunctions == std::vector<uint32_t>{0, 4});
    CHECK(stats.hot == 2);
    CHECK(stats.cold == 2);
}

TEST_CASE("placement: without a profile nothing moves") {
    il::Module mod = program();
    const lower::PlacementStats stats = lower::planFunctionPlacement(mod, nullptr);
    CHECK_FALSE(stats.profiled);
    CHECK(mod.hotFunctions.empty());
    CHECK(mod.coldFunctions.empty());

    // A plan that matched nothing (a stale harvest) is no profile.
    il::Module again = program();
    const spec::FeedbackPlan empty;
    CHECK_FALSE(lower::planFunctionPlacement(again, &empty).profiled);
    CHECK(again.hotFunctions.empty());
    CHECK(again.coldFunctions.empty());
}

TEST_CASE("placement: the same plan places the same way") {
    il::Module a = program(), b = program();
    const spec::FeedbackPlan plan = harvest();
    lower::planFunctionPlacement(a, &plan);
    lower::planFunctionPlacement(b, &plan);
    CHECK(a.hotFunctions == b.hotFunctions);
    CHECK(a.coldFunctions == b.coldFunctions);
}

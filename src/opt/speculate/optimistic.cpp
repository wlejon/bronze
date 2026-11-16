// The optimistic copy's own steps (optimistic.h has the design).

#include "opt/speculate/optimistic.h"

#include <algorithm>
#include <cassert>

#include "opt/core/cfg.h"
#include "opt/core/cfg_edit.h"

namespace bronze::opt::speculate {

std::vector<SitePlan> sitesOutsideLeftCode(const il::Function& generic, const FunctionPlan& plan) {
    if (plan.neverEntered.empty()) return plan.sites;
    const Cfg cfg(generic);
    const DomTree dom(cfg);
    std::vector<SitePlan> out;
    for (const SitePlan& site : plan.sites) {
        const bool left = std::any_of(plan.neverEntered.begin(), plan.neverEntered.end(),
                                      [&](BlockId b) { return dom.dominates(b, site.block); });
        if (!left) out.push_back(site);
    }
    return out;
}

uint32_t leaveNeverEntered(RewriteContext& ctx, const std::vector<BlockId>& blocks) {
    assert(ctx.optimistic);
    il::Function& s = ctx.speculated;
    uint32_t turned = 0;
    for (BlockId b : blocks) {
        assert(b < s.blocks.size() && !s.blocks[b].instructions.empty());
        const uint32_t id = ctx.resume.at(b, 0, true);
        il::Instruction leave;
        leave.op = il::Op::Leave;
        leave.type = il::Type::Void;
        leave.calleeIndex = ctx.genericIndex;
        leave.immI32 = static_cast<int32_t>(id);
        leave.keyIndex = il::kResumeOptimistic;
        // The point's state is liveBefore(G, b, 0): b's parameters and values
        // from blocks dominating b, which S+ has under G's ids there.
        leave.operands = ctx.resume.state(id);
        leave.span = s.blocks[b].instructions.front().span;
        s.blocks[b].instructions.clear();
        s.blocks[b].instructions.push_back(std::move(leave));
        s.blocks[b].cold = true;
        ++turned;
    }
    ctx.optimisticExits += turned;
    if (turned != 0) removeUnreachableBlocks(s);
    return turned;
}

}  // namespace bronze::opt::speculate

// MEMBERS ABOVE THE RECEIVER (member_call.cpp is the rewrite): a harvested
// site each of whose ways found its key on the receiver's prototype chain and
// names the function it reached there.
//
//   MemberMethod  a method site whose ways are all `direct` (the method cache
//                 latched a receiver shape and the code its chain's data
//                 property held; an own-slot way is `slot`);
//   MemberGet     a read site whose ways are all `accessor`, by the getters
//                 they name (harvest version 7);
//   MemberSet     a write site whose ways are all `accessor`, by the setters.
//
// One function per way, in the harvest's order; at most kMaxChainShapes ways,
// none of them `*other*`, and a site that met receivers its ways could not
// hold (`full`, `mega`, an `other` row) is left generic. Each function must
// be one module function a direct call with `argc` arguments may enter
// (policy.h directCallFits) without its function object: the fast path has
// the member cell's word in hand, not the closure, so a function that reads
// its environment record is refused.

#include <algorithm>

#include "opt/speculate/policy.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

std::optional<SitePlan> Evidence::planMember(const spec::HarvestSite& h, SiteKind kind, size_t argc, BlockId block,
                                             uint32_t index) const {
    if (h.full || h.mega || h.uncached > kMaxUncachedReceivers) return std::nullopt;
    if (h.ways.empty() || h.ways.size() > kMaxChainShapes) return std::nullopt;
    const char* form = kind == SiteKind::MemberMethod ? "direct" : "accessor";
    SitePlan plan;
    plan.kind = kind;
    plan.block = block;
    plan.index = index;
    for (const spec::HarvestWay& way : h.ways) {
        if (way.form != form) return std::nullopt;
        const std::string& name = kind == SiteKind::MemberSet ? way.setter : way.callee;
        if (name.empty() || name == "*other*") return std::nullopt;
        const uint32_t f = calleeByHarvestName(name);
        if (f == kNoFunction) return std::nullopt;
        const il::Function& callee = module_.functions[f];
        if (!directCallFits(callee, argc) || callee.needsEnv) return std::nullopt;
        plan.callees.push_back(f);
        plan.shapes.push_back(ShapeSlot{way.receiver, 0});
    }
    return plan;
}

}  // namespace bronze::opt::speculate

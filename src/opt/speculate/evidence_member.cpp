// MEMBERS ABOVE THE RECEIVER (member_call.cpp is the rewrite): a harvested
// site each of whose ways found its key on the receiver's prototype chain and
// names the function it reached there, or each of whose ways found the key
// nowhere.
//
//   MemberMethod  a method site whose ways are all `direct` (the method cache
//                 latched a receiver shape and the code its chain's data
//                 property held; an own-slot way is `slot`);
//   MemberGet     a read site whose ways are all `accessor`, by the getters
//                 they name (harvest version 7);
//   MemberSet     a write site whose ways are all `accessor`, by the setters;
//   MemberAbsent  a read site whose ways are all `absent` (no function);
//   MemberData    a read site some of whose ways are `proto`, each naming
//                 the depth and slot of its holder (harvest version 8), the
//                 rest `own` or `absent` (member_data.cpp).
//
// One function per way, in the harvest's order; at most kMaxSetShapes ways,
// none of them `*other*`, and a site that met receivers its ways could not
// hold (`full`, `mega`, an `other` row) is left generic. Each function must
// be one module function a direct call with `argc` arguments may enter
// (policy.h directCallFits) without its function object: the fast path has
// the member cell's word in hand, not the closure, so a function that reads
// its environment record is refused.

#include <algorithm>
#include <charconv>

#include "opt/core/shape_key.h"
#include "opt/speculate/policy.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

std::optional<SitePlan> Evidence::planMember(const spec::HarvestSite& h, SiteKind kind, size_t argc, BlockId block,
                                             uint32_t index) const {
    if (h.full || h.mega || h.uncached > kMaxUncachedReceivers) return std::nullopt;
    if (h.ways.empty() || h.ways.size() > kMaxSetShapes) return std::nullopt;
    const char* form = kind == SiteKind::MemberMethod ? "direct" : kind == SiteKind::MemberAbsent ? "absent" : "accessor";
    SitePlan plan;
    plan.kind = kind;
    plan.block = block;
    plan.index = index;
    for (const spec::HarvestWay& way : h.ways) {
        if (way.form != form) return std::nullopt;
        if (kind == SiteKind::MemberAbsent) {
            plan.callees.push_back(il::Module::kNoMemberCallee);
            plan.shapes.push_back(ShapeSlot{way.receiver, 0});
            continue;
        }
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

// A read whose ways found the key above the receiver, beside the receiver or
// nowhere: one entry per way, in the harvest's order, under the limits of the
// member sites above (no receiver the ways could not hold, at most
// kMaxSetShapes ways), except that a `full` read is planned too, open: its
// ways sample its receivers, and one they do not name is read generically
// without leaving. A `proto` way must name its holder within
// kMaxMemberDataDepth links, an `own` way a shape whose name places the key.
std::optional<SitePlan> Evidence::planMemberData(const spec::HarvestSite& h, const std::string& key,
                                                 BlockId block, uint32_t index) const {
    if (h.mega || h.uncached > kMaxUncachedReceivers) return std::nullopt;
    if (h.ways.empty() || h.ways.size() > kMaxSetShapes) return std::nullopt;
    auto number = [](const std::string& text, uint32_t& out) {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc() && end == text.data() + text.size();
    };
    SitePlan plan;
    plan.kind = SiteKind::MemberData;
    plan.block = block;
    plan.index = index;
    plan.calleesOpen = h.full;
    for (const spec::HarvestWay& way : h.ways) {
        if (way.form == "absent") {
            plan.callees.push_back(il::Module::kNoMemberCallee);
            plan.shapes.push_back(ShapeSlot{way.receiver, 0});
        } else if (way.form == "own") {
            const std::optional<ShapeName> shape = parseShapeName(way.receiver);
            if (!shape) return std::nullopt;
            const std::optional<int32_t> slot = ownSlot(*shape, key);
            if (!slot) return std::nullopt;
            plan.callees.push_back(kMemberDataOwn);
            plan.shapes.push_back(ShapeSlot{way.receiver, *slot, numberSlot(*shape, key)});
        } else if (way.form == "proto") {
            uint32_t depth = 0;
            uint32_t slot = 0;
            if (!number(way.callee, depth) || !number(way.setter, slot) || depth == 0 ||
                depth > kMaxMemberDataDepth || slot >= (1u << il::Module::kMemberDataSlotBits)) {
                return std::nullopt;
            }
            plan.callees.push_back(il::Module::memberDataPlace(depth, slot));
            plan.shapes.push_back(ShapeSlot{way.receiver, 0});
        } else {
            return std::nullopt;
        }
    }
    return plan;
}

}  // namespace bronze::opt::speculate

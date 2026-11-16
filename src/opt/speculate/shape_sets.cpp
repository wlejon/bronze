// SHAPE SETS: what a polymorphic miss does, chosen per receiver value.
//
// A method that reads and writes a dozen fields of `this` on three receiver
// shapes would otherwise test each access against the three shapes again: a
// chain's last miss makes the generic access in a cold arm and rejoins
// (prop_read.cpp), and the join erases what the chain learnt of the object.
// So the PropRead and PropWrite plans of one function are grouped by their
// receiver value (S keeps G's value ids), and in each group:
//
//   - a site that met two or more shapes, is dominated by no other site of
//     the group and dominates at least one becomes the LEAD. Its test is
//     "the shape is one of the set" and a miss LEAVES to G, so on its pass
//     edge the receiver is known to be one of the set (edge_facts.h
//     ShapeSet). The set is every shape the lead and the sites it dominates
//     met, less those the lead's own key is not an own (for a write,
//     writable) slot of, at most kMaxSetShapes;
//   - every site it dominates is a FOLLOWER: its shapes are widened by the
//     set's shapes that hold its key, and it tests the set with a cold
//     generic arm. Where the lead's fact still holds (no write of Shapes
//     since) the redundancy pass folds that test, and with one slot for all
//     the shapes the access is one slot load or store;
//   - a site of a group with no lead keeps its chain and its generic arm.
//
// A lead that keeps missing (a shape the harvest never showed) is a guard
// like any other: its exits are counted and past the limit S retires.
// A receiver with a single site keeps the generic arm: a set guard there
// would buy nothing to fold.

#include <algorithm>
#include <unordered_map>

#include "opt/core/cfg.h"
#include "opt/core/shape_key.h"
#include "opt/speculate/evidence.h"

namespace bronze::opt::speculate {

namespace {

bool isOwnSlotSite(const SitePlan& p) {
    return (p.kind == SiteKind::PropRead || p.kind == SiteKind::PropWrite) && !p.shapes.empty();
}

// The slot `key` has in the shape named `name` for an access of `kind`.
std::optional<int32_t> slotFor(SiteKind kind, const std::string& name, const std::string& key) {
    const std::optional<ShapeName> shape = parseShapeName(name);
    if (!shape) return std::nullopt;
    return kind == SiteKind::PropWrite ? writableSlot(*shape, key) : ownSlot(*shape, key);
}

bool hasShape(const SitePlan& p, const std::string& name) {
    return std::any_of(p.shapes.begin(), p.shapes.end(), [&](const ShapeSlot& s) { return s.key == name; });
}

}  // namespace

void Evidence::shareShapeSets(const il::Function& fn, FunctionPlan& plan) const {
    std::unordered_map<ValueId, std::vector<size_t>> byReceiver;
    for (size_t i = 0; i < plan.sites.size(); ++i) {
        const SitePlan& p = plan.sites[i];
        if (!isOwnSlotSite(p)) continue;
        byReceiver[fn.blocks[p.block].instructions[p.index].operands[0]].push_back(i);
    }
    if (std::none_of(byReceiver.begin(), byReceiver.end(), [](const auto& g) { return g.second.size() > 1; })) {
        return;
    }
    const Cfg cfg(fn);
    const DomTree dom(cfg);
    auto dominates = [&](const SitePlan& a, const SitePlan& b) {
        if (a.block == b.block) return a.index < b.index;
        return dom.dominates(a.block, b.block);
    };
    auto keyOf = [&](const SitePlan& p) -> const std::string& {
        return module_.keyConstants[fn.blocks[p.block].instructions[p.index].keyIndex];
    };

    for (auto& [receiver, sites] : byReceiver) {
        if (sites.size() < 2) continue;
        for (size_t lead : sites) {
            SitePlan& l = plan.sites[lead];
            if (l.shapes.size() < 2 || l.shapeSet != ShapeSetRole::None) continue;
            const bool dominated = std::any_of(sites.begin(), sites.end(), [&](size_t o) {
                return o != lead && dominates(plan.sites[o], l);
            });
            if (dominated) continue;
            std::vector<size_t> followers;
            for (size_t o : sites) {
                if (o != lead && dominates(l, plan.sites[o])) followers.push_back(o);
            }
            if (followers.empty()) continue;

            // The set: the shapes every site met that hold the lead's key.
            std::vector<ShapeSlot> set;
            auto join = [&](const SitePlan& p) {
                for (const ShapeSlot& s : p.shapes) {
                    if (std::any_of(set.begin(), set.end(), [&](const ShapeSlot& t) { return t.key == s.key; })) {
                        continue;
                    }
                    if (const std::optional<int32_t> slot = slotFor(l.kind, s.key, keyOf(l))) {
                        set.push_back(ShapeSlot{s.key, *slot});
                    }
                }
            };
            join(l);
            for (size_t o : followers) join(plan.sites[o]);
            if (set.size() < 2 || set.size() > kMaxSetShapes) continue;

            l.shapes = set;
            l.shapeSet = ShapeSetRole::Lead;
            for (size_t o : followers) {
                SitePlan& f = plan.sites[o];
                for (const ShapeSlot& s : set) {
                    if (hasShape(f, s.key) || f.shapes.size() >= kMaxSetShapes) continue;
                    if (const std::optional<int32_t> slot = slotFor(f.kind, s.key, keyOf(f))) {
                        f.shapes.push_back(ShapeSlot{s.key, *slot});
                    }
                }
                // A site still of one shape keeps its leaving guard.
                if (f.shapes.size() > 1) f.shapeSet = ShapeSetRole::Follow;
            }
        }
    }
}

}  // namespace bronze::opt::speculate

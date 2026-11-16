// The shape test every own-slot rewrite begins with:
//
//     t = is.object %obj          ; guard: an object, so its shape word exists
//     s = load.shape %obj
//     c = shape.cell <n>          ; the module's cell for the profile's name
//     e = ptr.eq s, c             ; guard: exactly that shape
//
// `is.object` is total and pure and `load.shape` reads the header word every
// object kind has at BRONZE_ABI_OBJ_SHAPE_OFFSET, so nothing here is
// observable: a failure at either guard leaves before any effect. The compare
// stays in the block of its branch with the load, which is where edge_facts
// recognises it as a shape fact.

#include <algorithm>

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

ValueId emitShapeOf(GuardBuilder& b, ValueId object) {
    assert(b.typeOf(object) == il::Type::Dynamic);
    const ValueId isObject = b.emit(il::Op::IsObject, il::Type::Bool, {object});
    b.guard(isObject);
    return b.emit(il::Op::LoadShape, il::Type::Ptr, {object});
}

ValueId emitShapeIs(GuardBuilder& b, ValueId shape, const std::string& key) {
    const uint32_t cell = b.context().cells.cellFor(key);
    const ValueId expected = b.emit(il::Op::ShapeCell, il::Type::Ptr, {}, static_cast<int32_t>(cell));
    return b.emit(il::Op::PtrEq, il::Type::Bool, {shape, expected});
}

ValueId emitShapeIn(GuardBuilder& b, ValueId shape, const std::vector<std::string>& keys) {
    if (keys.size() == 1) return emitShapeIs(b, shape, keys.front());
    std::vector<ValueId> operands{shape};
    for (const std::string& key : keys) {
        const uint32_t cell = b.context().cells.cellFor(key);
        operands.push_back(b.emit(il::Op::ShapeCell, il::Type::Ptr, {}, static_cast<int32_t>(cell)));
    }
    return b.emit(il::Op::ShapeIn, il::Type::Bool, std::move(operands));
}

void guardShape(GuardBuilder& b, ValueId object, const std::string& key) {
    const ValueId shape = emitShapeOf(b, object);
    b.guard(emitShapeIs(b, shape, key));
}


// THE SLOT DISPATCH of an own-slot site on several shapes (prop_read.cpp,
// prop_write.cpp). Its shapes are grouped by the slot the key has in them (a
// subclass's shape extends its base's, so a base field usually sits at one
// slot in all of them), one access per group, each jumping to a join whose
// parameter is the site's result.
//
// A CHAIN (no shape-set role, or a follower whose shapes place the key at
// several slots): a `ptr.eq` per shape to its group's access; the last miss
// makes the generic access in a cold arm and rejoins (a polymorphic site
// meeting one more class stays in S).
//
//     s = load.shape %obj
//     br (ptr.eq s, c1), A1, N1
// N1: br (ptr.eq s, c2), A2, G
//  G: the generic access (cold), jump J
//
// A SET (a lead, or a follower whose shapes all place the key at one slot,
// shape_sets.cpp): one `shape.in` of every shape, then a test per group but
// the last. A lead's miss leaves in front of the site; a follower's takes the
// generic arm.
//
//     s = load.shape %obj
//     br (shape.in s, c1, ..., cn), D, X        X: leave, or the generic arm
//  D: br (shape.in s, <group 1>), A1, A2        (one group: D is A1)
//
// Nothing before the last guard is observable (tests and header loads), so a
// lead's exit re-runs the generic site in G with nothing done.
void emitSlotDispatch(GuardBuilder& b, const SitePlan& plan, ValueId object,
                      const std::function<ValueId(int32_t)>& access) {
    assert(plan.shapes.size() >= 2 && plan.shapes.size() <= kMaxSetShapes);
    const bool result = b.site().result != il::kNoValue;
    std::vector<std::pair<int32_t, std::vector<std::string>>> groups;
    for (const ShapeSlot& s : plan.shapes) {
        auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == s.slot; });
        if (g == groups.end()) {
            groups.push_back({s.slot, {s.key}});
        } else {
            g->second.push_back(s.key);
        }
    }
    if (plan.shapeSet == ShapeSetRole::Lead) ++b.context().stats.shapeSetLeads;
    if (plan.shapeSet == ShapeSetRole::Follow) ++b.context().stats.shapeSetFollowers;
    const bool set = plan.shapeSet == ShapeSetRole::Lead ||
                     (plan.shapeSet == ShapeSetRole::Follow && groups.size() == 1);

    const ValueId shape = emitShapeOf(b, object);
    const BlockId join = b.newBlock();
    std::vector<BlockId> accesses;
    for (size_t g = 0; g < groups.size(); ++g) accesses.push_back(b.newBlock());
    auto groupOf = [&](const std::string& key) {
        for (size_t g = 0; g < groups.size(); ++g) {
            if (std::find(groups[g].second.begin(), groups[g].second.end(), key) != groups[g].second.end()) {
                return accesses[g];
            }
        }
        return il::kNoBlock;
    };

    if (set) {
        std::vector<std::string> keys;
        for (const ShapeSlot& s : plan.shapes) keys.push_back(s.key);
        const ValueId in = emitShapeIn(b, shape, keys);
        if (plan.shapeSet == ShapeSetRole::Lead) {
            b.guard(in);
        } else {
            const BlockId other = b.genericArm(join);
            const BlockId pass = b.newBlock();
            b.branch(in, pass, {}, other, {});
            b.moveTo(pass);
        }
        for (size_t g = 0; g + 1 < groups.size(); ++g) {
            const BlockId next = g + 2 < groups.size() ? b.newBlock() : accesses[g + 1];
            b.branch(emitShapeIn(b, shape, groups[g].second), accesses[g], {}, next, {});
            if (next != accesses[g + 1]) b.moveTo(next);
        }
        if (groups.size() == 1) b.jump(accesses[0]);
    } else {
        const BlockId other = b.genericArm(join);
        for (size_t i = 0; i < plan.shapes.size(); ++i) {
            const bool last = i + 1 == plan.shapes.size();
            const BlockId next = last ? other : b.newBlock();
            b.branch(emitShapeIs(b, shape, plan.shapes[i].key), groupOf(plan.shapes[i].key), {}, next, {});
            if (!last) b.moveTo(next);
        }
    }
    for (size_t g = 0; g < groups.size(); ++g) {
        b.moveTo(accesses[g]);
        const ValueId v = access(groups[g].first);
        b.jump(join, result ? std::vector<ValueId>{v} : std::vector<ValueId>{});
    }
    if (result) b.resultParam(join);
    b.moveTo(join);
}

}  // namespace bronze::opt::speculate

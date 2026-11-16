// SLOT-KEYED PROPERTY ACCESS: `r = prop.get %obj, <key>` and the assignment
// `prop.set %obj, <key>, %v` on receivers that come in more shapes than a
// chain of identity tests covers.
//
// Evidence (evidence.cpp planSlotKeyed): the site's cache was full, or met
// receivers none of its ways could hold, and every shape its ways sampled
// keeps the key as an own data property (writable, for a write) at one of at
// most kMaxChainShapes slots. three.js's Object3D fields are the case: every
// subclass's constructor runs Object3D's first, so `this.matrixWorld` sits at
// one slot in a Bone, a Mesh and a light alike, on a dozen shapes.
//
//     br is.object %obj, L, G
//  L: s = load.shape %obj
//     br shape.has_own s, <k1>, "key", H1, N1    H1: r1 = slot.load %obj, k1 ; jump J(r1)
// N1: br shape.has_own s, <k2>, "key", H2, G     H2: r2 = slot.load %obj, k2 ; jump J(r2)
//  G: r3 = prop.get %obj, <key>, <a fresh IC> ; jump J(r3)            (cold)
//  J(r): ...
//
// and for a write the same with `slot.store %obj, k, %v` in each Hi and the
// write form of the test (writable, and the shape no prototype's), so the
// store is OrdinarySet's in-place write of an existing own data property: no
// setter can be reached and no prototype's member cell is invalidated. Any
// receiver the tests do not answer, a primitive included, takes the generic
// access in the cold arm and the function stays in S: nothing here leaves.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewritePropSlot(RewriteContext& ctx, const SitePlan& plan) {
    const bool write = plan.kind == SiteKind::PropSlotWrite;
    assert((write || plan.kind == SiteKind::PropSlotRead) && !plan.shapes.empty() &&
           plan.shapes.size() <= kMaxChainShapes);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(write ? site.op == il::Op::PropSet && !il::isDefineWrite(site) && site.result == il::kNoValue
                 : site.op == il::Op::PropGet && site.type == il::Type::Dynamic && site.result != il::kNoValue);
    const ValueId object = site.operands[0];

    const BlockId join = b.newBlock();
    const BlockId other = b.genericArm(join);
    if (!write) b.resultParam(join);

    const BlockId tests = b.newBlock();
    b.branch(b.emit(il::Op::IsObject, il::Type::Bool, {object}), tests, {}, other, {});
    b.moveTo(tests);
    const ValueId shape = b.emit(il::Op::LoadShape, il::Type::Ptr, {object});
    for (size_t i = 0; i < plan.shapes.size(); ++i) {
        const int32_t slot = plan.shapes[i].slot;
        il::Instruction test;
        test.op = il::Op::ShapeHasOwn;
        test.type = il::Type::Bool;
        test.operands = {shape};
        test.immI32 = slot * 2 + (write ? 1 : 0);
        test.keyIndex = site.keyIndex;
        const ValueId holds = b.emitInst(std::move(test));
        const bool last = i + 1 == plan.shapes.size();
        const BlockId hit = b.newBlock();
        const BlockId next = last ? other : b.newBlock();
        b.branch(holds, hit, {}, next, {});
        b.moveTo(hit);
        il::Instruction access;
        access.op = write ? il::Op::SlotStore : il::Op::SlotLoad;
        access.type = write ? il::Type::Void : il::Type::Dynamic;
        access.operands = write ? std::vector<ValueId>{object, site.operands[1]} : std::vector<ValueId>{object};
        access.immI32 = slot;
        const ValueId value = b.emitInst(std::move(access));
        b.jump(join, write ? std::vector<ValueId>{} : std::vector<ValueId>{value});
        if (!last) b.moveTo(next);
    }
    b.moveTo(join);
    b.finish();
    if (write) {
        ++ctx.stats.propWrites;
    } else {
        ++ctx.stats.propReads;
    }
    ++ctx.stats.propSlotKeyed;
}

}  // namespace bronze::opt::speculate

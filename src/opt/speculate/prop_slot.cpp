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
//
// ABSENT KEYS. A read whose ways also found the key nowhere (evidence.cpp
// planPropRead: more receivers than member cells cover, or own ways beside
// them) tests the shape's absent cache too, before the slots when most ways
// were absent ones:
//
//     br shape.lacks %obj, s, "key", U, N     U: jump J(undefined)
//
// `shape.lacks` is answered by the runtime for a plain receiver and recorded
// per shape under the prototype epoch (il_op.h ShapeLacks), so a shape the
// ways never saw is answered too, and a receiver it does not answer takes
// the generic arm.
//
// DOUBLE SLOTS (runtime/slot_repr.h). The table says which slots are double
// ones (BRONZE_ABI_SLOTKEY_NUMBER). A read tests the plain form, which takes
// either representation, unless every sampled shape keeps the key at that
// slot a double one: then it tests the Number form, and the load is a
// Number. A write first tests the form that takes either representation and
// stores a Number; any other value tests the boxed form:
//
//  T: br shape.has_own s, <k> write number, "key", N, T'
//  N: br is.number %v, H, B
//  B: br shape.has_own s, <k> write, "key", H, T'
//  H: slot.store %obj, k, %v ; jump J

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewritePropSlot(RewriteContext& ctx, const SitePlan& plan) {
    const bool write = plan.kind == SiteKind::PropSlotWrite;
    assert((write || plan.kind == SiteKind::PropSlotRead) && (!plan.shapes.empty() || plan.absent) &&
           plan.shapes.size() <= kMaxChainShapes && (!plan.absent || !write));
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
    auto hasOwn = [&](int32_t slot, int32_t mode) {
        il::Instruction test;
        test.op = il::Op::ShapeHasOwn;
        test.type = il::Type::Bool;
        test.operands = {shape};
        test.immI32 = (slot << il::kHasOwnSlotShift) | mode;
        test.keyIndex = site.keyIndex;
        return b.emitInst(std::move(test));
    };
    // The absent test, in front of the slot tests or behind them; `next` is
    // where its miss goes.
    auto lacks = [&](BlockId next) {
        il::Instruction test;
        test.op = il::Op::ShapeLacks;
        test.type = il::Type::Bool;
        test.operands = {object, shape};
        test.keyIndex = site.keyIndex;
        const ValueId absent = b.emitInst(std::move(test));
        const BlockId undef = b.newBlock();
        b.branch(absent, undef, {}, next, {});
        b.moveTo(undef);
        b.jump(join, {b.emit(il::Op::ConstUndefined, il::Type::Dynamic, {})});
        ++ctx.stats.propAbsentTests;
    };
    if (plan.absent && (plan.absentFirst || plan.shapes.empty())) {
        const BlockId next = plan.shapes.empty() ? other : b.newBlock();
        lacks(next);
        if (!plan.shapes.empty()) b.moveTo(next);
    }
    const bool lacksLast = plan.absent && !plan.absentFirst && !plan.shapes.empty();
    for (size_t i = 0; i < plan.shapes.size(); ++i) {
        const int32_t slot = plan.shapes[i].slot;
        const bool number = !write && plan.shapes[i].number;
        const int32_t mode = write ? il::kHasOwnWrite | il::kHasOwnNumber : number ? il::kHasOwnNumber : 0;
        const bool last = i + 1 == plan.shapes.size();
        const BlockId hit = b.newBlock();
        const BlockId next = last && !lacksLast ? other : b.newBlock();
        if (!write) {
            b.branch(hasOwn(slot, mode), hit, {}, next, {});
        } else {
            const BlockId numeric = b.newBlock();
            const BlockId boxed = b.newBlock();
            b.branch(hasOwn(slot, mode), numeric, {}, next, {});
            b.moveTo(numeric);
            b.branch(b.emit(il::Op::IsNumber, il::Type::Bool, {site.operands[1]}), hit, {}, boxed, {});
            b.moveTo(boxed);
            b.branch(hasOwn(slot, il::kHasOwnWrite), hit, {}, next, {});
        }
        b.moveTo(hit);
        il::Instruction access;
        access.op = write ? il::Op::SlotStore : il::Op::SlotLoad;
        access.type = write ? il::Type::Void : il::Type::Dynamic;
        access.operands = write ? std::vector<ValueId>{object, site.operands[1]} : std::vector<ValueId>{object};
        access.immI32 = slot;
        if (number) {
            access.boxType = il::Type::F64;
            ++ctx.stats.numberSlotReads;
        }
        const ValueId value = b.emitInst(std::move(access));
        b.jump(join, write ? std::vector<ValueId>{} : std::vector<ValueId>{value});
        if (!last || lacksLast) b.moveTo(next);
    }
    if (lacksLast) lacks(other);
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

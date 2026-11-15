// PROPERTY READ: `r = prop.get %obj, <key>` on own data slots.
//
// Evidence (evidence.cpp planPropRead): every receiver was a plain object of
// one of 1..kMaxChainShapes shapes, each holding the key as an OWN property —
// a harvest's `own` ways, or a recorded Shape site's names that list the key.
// The slot is the key's position by the slot rule (shape_key.h), and the
// shape cell's key is the profile's name verbatim.
//
// One shape:
//     guard is.object %obj
//     guard ptr.eq (load.shape %obj), (shape.cell n)
//     r = slot.load %obj, k
//
// A chain (2..kMaxChainShapes shapes): one `is.object` guard and one shape
// load, then a test per shape; each passing test loads its own slot and jumps
// to a join whose parameter is `r`; the LAST failing test leaves.
//
//     s = load.shape %obj
//     br (ptr.eq s, c1), L1, N1      L1: v1 = slot.load %obj, k1; jump J(v1)
//  N1: br (ptr.eq s, c2), L2, exit   L2: v2 = slot.load %obj, k2; jump J(v2)
//   J(r): ...
//
// Why exact: the loader resolves a cell only to a shape whose every property
// is a plain writable data property in the boxed representation (shape_key.h),
// so for an object at that shape [[Get]] of the key is its own data property,
// found without a prototype walk, a getter or a proxy trap — the slot's
// Value. Nothing before the last guard is observable (tests and header
// loads), so a failure re-runs the generic `prop.get` in G with nothing done.
// `proto` ways (the value lives on a prototype object this step has no
// constant for), `absent`, `accessor` and `array` ways stay generic.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewritePropRead(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::PropRead && !plan.shapes.empty() && plan.shapes.size() <= kMaxChainShapes);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::PropGet && site.type == il::Type::Dynamic && site.result != il::kNoValue);
    const ValueId object = site.operands[0];

    auto slotLoad = [&](int32_t slot) {
        il::Instruction load;
        load.op = il::Op::SlotLoad;
        load.type = il::Type::Dynamic;
        load.operands = {object};
        load.immI32 = slot;
        return load;
    };

    if (plan.shapes.size() == 1) {
        guardShape(b, object, plan.shapes.front().key);
        b.emitResult(slotLoad(plan.shapes.front().slot));
        b.finish();
        ++ctx.stats.propReads;
        return;
    }

    const ValueId shape = emitShapeOf(b, object);
    const BlockId join = b.newBlock();
    b.resultParam(join);
    for (size_t i = 0; i < plan.shapes.size(); ++i) {
        const ValueId matches = emitShapeIs(b, shape, plan.shapes[i].key);
        BlockId next = il::kNoBlock;
        if (i + 1 == plan.shapes.size()) {
            // The last test is the guard: no shape matched, so leave. The
            // cursor continues at its passing block.
            b.guard(matches);
        } else {
            const BlockId hit = b.newBlock();
            next = b.newBlock();
            b.branch(matches, hit, {}, next, {});
            b.moveTo(hit);
        }
        const ValueId value = b.emitInst(slotLoad(plan.shapes[i].slot));
        b.jump(join, {value});
        if (next != il::kNoBlock) b.moveTo(next);
    }
    b.moveTo(join);
    b.finish();
    ++ctx.stats.propReads;
    ++ctx.stats.propReadChains;
}

}  // namespace bronze::opt::speculate

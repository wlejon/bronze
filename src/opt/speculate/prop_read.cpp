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
// Several shapes (2..kMaxChainShapes, or a receiver's shape set): one
// `is.object` guard and one shape load, then the slot dispatch of
// prop_shape.cpp emitSlotDispatch, a `slot.load` per distinct slot jumping to
// a join whose parameter is `r`.
//
// Why exact: the loader resolves a cell only to a shape whose every property
// is a plain data property in the representation its name spells
// (shape_key.h), so for an object at that shape [[Get]] of the key is its own
// data property, found without a prototype walk, a getter or a proxy trap —
// the slot's Value, a Number when the name spells the key `%k`. Nothing
// before the last guard is observable (tests and header loads), so a failure
// re-runs the generic `prop.get` in G with nothing done.
// `proto` ways (the value lives on a prototype object this step has no
// constant for), `accessor` and `array` ways stay generic; `absent` ways are
// member cells (member_call.cpp) or, beside own ways or past what cells
// cover, a slot-keyed read's `shape.lacks` arm (prop_slot.cpp).

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewritePropRead(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::PropRead && !plan.shapes.empty() && plan.shapes.size() <= kMaxSetShapes);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::PropGet && site.type == il::Type::Dynamic && site.result != il::kNoValue);
    const ValueId object = site.operands[0];

    // A slot every shape that reaches the load keeps a double one is read
    // as a Number (il_op.h SlotLoad `number`).
    auto slotLoad = [&](int32_t slot, bool number) {
        il::Instruction load;
        load.op = il::Op::SlotLoad;
        load.type = il::Type::Dynamic;
        load.operands = {object};
        load.immI32 = slot;
        if (number) {
            load.boxType = il::Type::F64;
            ++ctx.stats.numberSlotReads;
        }
        return load;
    };

    if (plan.shapes.size() == 1) {
        guardShape(b, object, plan.shapes.front().key);
        b.emitResult(slotLoad(plan.shapes.front().slot, holdsNumber(b, plan.shapes.front())));
        b.finish();
        ++ctx.stats.propReads;
        return;
    }

    emitSlotDispatch(b, plan, object,
                     [&](const SlotGroup& g, BlockId) { return b.emitInst(slotLoad(g.slot, g.allNumber)); });
    b.finish();
    ++ctx.stats.propReads;
    ++ctx.stats.propReadChains;
}

}  // namespace bronze::opt::speculate

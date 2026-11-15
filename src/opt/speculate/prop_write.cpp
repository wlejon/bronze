// PROPERTY WRITE: `prop.set %obj, <key>, %v` (an assignment, sloppy or
// strict) to a property the receiver already has.
//
// Evidence (evidence.cpp planPropWrite): one shape, holding the key as an own
// property before the write — a harvest's single `store` way (a write the
// cache filled for a key the receiver had), or a recorded Shape site, which
// observes the receiver before the write, naming one shape that lists the
// key. A write that ADDED the key (the harvest's `own` form for a write site:
// a shape transition) and a define (kWriteDefine: object literals and class
// fields, CreateDataPropertyOrThrow) stay generic.
//
//     guard is.object %obj
//     guard ptr.eq (load.shape %obj), (shape.cell n)
//     slot.store %obj, k, %v
//
// Why exact: for an object at the cell's shape the key is an own data
// property that is WRITABLE (the loader contract, shape_key.h: every property
// of a resolved shape has the attributes an assignment creates; a read-only
// or accessor property makes another shape of the same name, which the cell
// never resolves to), so 10.1.9 OrdinarySet writes the slot and nothing else:
// no setter, no prototype walk (an own data property is found first), no
// refusal in either mode, no shape change, and the slot is boxed so any Value
// may go in. `slot.store` carries the write barrier. Nothing before the store
// is observable, so a failed guard re-runs the generic `prop.set` in G.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewritePropWrite(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::PropWrite && plan.shapes.size() == 1);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::PropSet && !il::isDefineWrite(site) && site.result == il::kNoValue);
    const ValueId object = site.operands[0];
    const ValueId value = site.operands[1];

    guardShape(b, object, plan.shapes.front().key);
    il::Instruction store;
    store.op = il::Op::SlotStore;
    store.type = il::Type::Void;
    store.operands = {object, value};
    store.immI32 = plan.shapes.front().slot;
    b.emitInst(std::move(store));
    b.finish();
    ++ctx.stats.propWrites;
}

}  // namespace bronze::opt::speculate

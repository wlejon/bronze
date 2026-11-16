// PROPERTY WRITE: `prop.set %obj, <key>, %v` (an assignment, sloppy or
// strict) to a property the receiver already has.
//
// Evidence (evidence.cpp planPropWrite): one to kMaxChainShapes shapes (each
// tested in turn, a store per distinct slot), holding the key as an own
// property before the write — a harvest's single `store` way (a write the
// cache filled for a key the receiver had), or a recorded Shape site, which
// observes the receiver before the write, naming one shape that lists the
// key. A write that ADDED the key (the harvest's `own` form for a write site:
// a shape transition) is the PROPERTY ADD below; a define (kWriteDefine:
// class fields, CreateDataPropertyOrThrow) stays generic.
//
//     guard is.object %obj
//     guard ptr.eq (load.shape %obj), (shape.cell n)
//     slot.store %obj, k, %v
//
// On 2..kMaxChainShapes shapes (or a receiver's shape set, shape_sets.cpp) the
// shape test is the slot dispatch of prop_shape.cpp, a store per
// distinct slot, and a receiver of none of them makes the generic `prop.set`
// in a cold arm and continues in S (the site is polymorphic: one more class
// is incomplete evidence, not wrong evidence).
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

//
// PROPERTY ADD: the same assignment where the key was ADDED (a harvest's
// single `own` way for a write site, evidence.cpp planPropAdd), landing at an
// inline slot:
//
//     guard is.object %obj
//     guard ptr.eq (load.shape %obj), (shape.cell from)
//         failing: ptr.eq (load.shape %obj), (shape.cell to) ? slot.store %obj, k, %v
//                                                             : leave
//     guard proto.cell c                  ; exit: proto.publish c, then leave
//     slot.store %obj, k, %v
//     shape.store %obj, (shape.cell to)
//
// The `to` arm (cold) is a PROPERTY WRITE: the cache way an add fills is the
// one every later store to the key on an object at `to` hits, so one `own`
// way stands for both.
// Why exact: an object at `from` is a plain, extensible object (a
// non-extensible one is a dictionary, which no cell resolves to) without the
// key. Cell c (il.h Module::protoCells, runtime/transition_cells.cpp) is
// stamped with the current prototype epoch only once the runtime checked that
// `to` is `from`'s child for the key, a boxed writable enumerable
// configurable data property at inline slot k, that no object is a prototype
// through `from` (an add to one must bump the epoch), and that the chain above
// has no setter, read-only property or proxy for the key; every change to a
// link of that chain bumps the epoch. So 10.1.9 OrdinarySet reaches
// CreateDataProperty on the receiver, which is the store and the shape word.
// The object's inline slot k exists (its capacity is `from`'s), and nothing
// between the two stores can collect. The publishing exit runs no user code
// and changes nothing G can see, so leaving in front of the site is exact.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

uint32_t protoCellFor(il::Module& module, const il::Module::ProtoCell& cell) {
    for (size_t i = 0; i < module.protoCells.size(); ++i) {
        if (module.protoCells[i] == cell) return static_cast<uint32_t>(i);
    }
    module.protoCells.push_back(cell);
    return static_cast<uint32_t>(module.protoCells.size() - 1);
}

}  // namespace

void rewritePropAdd(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::PropAdd && plan.shapes.size() == 2);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::PropSet && !il::isDefineWrite(site) && site.result == il::kNoValue);
    const ValueId object = site.operands[0];
    const ValueId value = site.operands[1];
    const ShapeSlot& from = plan.shapes[0];
    const ShapeSlot& to = plan.shapes[1];

    il::Module::ProtoCell cell;
    cell.fromCell = ctx.cells.cellFor(from.key);
    cell.keyIndex = site.keyIndex;
    cell.toCell = ctx.cells.cellFor(to.key);
    cell.slot = to.slot;
    const auto c = static_cast<int32_t>(protoCellFor(ctx.module, cell));

    // A receiver already at `to` stores into the slot, as a PROPERTY WRITE
    // does: the way an add fills is the one later stores to the key hit, so
    // the harvest's `own` way names both. That arm is off the add's failing
    // edge, cold, so an add the redundancy pass proves (a constructor's run of
    // adds) keeps one test.
    const ValueId shape = emitShapeOf(b, object);
    const BlockId adds = b.newBlock();
    const BlockId stored = b.newBlock();
    const BlockId stores = b.newBlock();
    const BlockId join = b.newBlock();
    b.context().speculated.blocks[stored].cold = true;
    b.context().speculated.blocks[stores].cold = true;
    b.branch(emitShapeIs(b, shape, from.key), adds, {}, stored, {});
    b.moveTo(stored);
    b.branch(emitShapeIs(b, b.emit(il::Op::LoadShape, il::Type::Ptr, {object}), to.key), stores, {},
             b.exitBefore(), {});
    b.moveTo(stores);
    {
        il::Instruction store;
        store.op = il::Op::SlotStore;
        store.type = il::Type::Void;
        store.operands = {object, value};
        store.immI32 = to.slot;
        b.emitInst(std::move(store));
    }
    b.jump(join);
    b.moveTo(adds);
    il::Instruction publish;
    publish.op = il::Op::ProtoPublish;
    publish.type = il::Type::Void;
    publish.immI32 = c;
    std::vector<il::Instruction> work;
    work.push_back(std::move(publish));
    b.guardTo(b.emit(il::Op::ProtoCell, il::Type::Bool, {}, c), true, b.exitBeforeWith(std::move(work)));

    il::Instruction store;
    store.op = il::Op::SlotStore;
    store.type = il::Type::Void;
    store.operands = {object, value};
    store.immI32 = to.slot;
    b.emitInst(std::move(store));
    const ValueId toShape = b.emit(il::Op::ShapeCell, il::Type::Ptr, {}, static_cast<int32_t>(cell.toCell));
    b.emit(il::Op::ShapeStore, il::Type::Void, {object, toShape});
    b.jump(join);
    b.moveTo(join);
    b.finish();
    ++ctx.stats.propAdds;
}

void rewritePropWrite(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::PropWrite && !plan.shapes.empty() && plan.shapes.size() <= kMaxSetShapes);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction& site = b.site();
    assert(site.op == il::Op::PropSet && !il::isDefineWrite(site) && site.result == il::kNoValue);
    const ValueId object = site.operands[0];
    const ValueId value = site.operands[1];
    auto slotStore = [&](int32_t slot) {
        il::Instruction store;
        store.op = il::Op::SlotStore;
        store.type = il::Type::Void;
        store.operands = {object, value};
        store.immI32 = slot;
        b.emitInst(std::move(store));
    };

    if (plan.shapes.size() == 1) {
        guardShape(b, object, plan.shapes.front().key);
        slotStore(plan.shapes.front().slot);
        b.finish();
        ++ctx.stats.propWrites;
        return;
    }

    // Several shapes: the slot dispatch (prop_shape.cpp), one store per
    // distinct slot.
    emitSlotDispatch(b, plan, object, [&](int32_t slot) {
        slotStore(slot);
        return il::kNoValue;
    });
    b.finish();
    ++ctx.stats.propWrites;
}

}  // namespace bronze::opt::speculate

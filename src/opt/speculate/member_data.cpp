// A PROTOTYPE'S DATA: `r = prop.get %obj, <key>` whose harvest found the key
// as a data property above some receivers (`proto` ways, each naming the
// links to its holder and the holder's slot), and own or nowhere for the
// rest (evidence_member.cpp planMemberData): a class's constant on its
// prototype (`gl.TEXTURE_WRAP_S`), a default an instance may override
// (`this.scale`). Each way is one test of the receiver's shape:
//
//     guard is.object %obj
//     s = load.shape %obj
//     br (ptr.eq s, (shape.cell n)), own(k), next1          ; an `own` way
//   next1:
//     br (ptr.eq s, (member.cell c1)), data(d, k'), next2   ; a `proto` way
//   next2:
//     br (ptr.eq s, (member.cell c2)), undef, exit          ; an `absent` way
//   own(k):     r = slot.load %obj, k ; jump join(r)
//   data(d,k'): p1 = shape.proto s                          ; d links up
//               p2 = shape.proto (load.shape p1) ...
//               r = slot.load pd, k' ; jump join(r)
//   undef:      jump join(undefined)
//   exit [cold]:
//     member.publish %obj, c1 ; member.publish %obj, c2
//     leave @G, <point in front of the site>, state
//
// or, past kMaxChainShapes ways or for a site that met more receivers than
// its ways name (SitePlan::calleesOpen), a cold arm doing the generic read
// after the publishes (the site stays in S), as member_call.cpp does. Ways
// of one place (an own slot, a holder's depth and slot, or `undefined`)
// share one arm.
//
// Why exact. An own way is prop_read.cpp's (the shape cell resolves only to
// a shape whose every property is plain data, in the representation its name
// spells). A data cell holds a shape only while the runtime's check stands
// (runtime/accessor_cell.cpp, kind 4): a plain receiver of that shape lacks
// the key as its own, and its chain, at the thread's prototype epoch, has
// the key on no link below a plain, non-dictionary holder `d` links up,
// which keeps it as a data property at slot k'. Everything that could make
// the chain answer otherwise (an add to any link, which may shadow; a delete
// or a dictionary demotion; an accessor defined over the property; a
// prototype swap) bumps the epoch, and a bump empties the cell. A WRITE of
// the value moves no shape and bumps nothing, and needs neither: the arm
// loads the slot every time it runs, through the links as they are (a
// collection that moved a prototype updated the root's word `shape.proto`
// reads), so a program that writes a prototype's data often costs no cell
// anything. Either representation of the slot reads as the Value it holds
// (runtime/slot_repr.h), so the load is a boxed one. Before the arm's load
// nothing observable has run (tests, header and cell loads), so a receiver no
// way holds leaves in front of the site, and G performs the whole read.

#include <map>

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteMemberData(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::MemberData && !plan.callees.empty() && plan.callees.size() <= kMaxSetShapes &&
           plan.callees.size() == plan.shapes.size());
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(site.op == il::Op::PropGet && site.operands.size() == 1 && site.result != il::kNoValue);
    const ValueId receiver = site.operands[0];
    il::Module& module = ctx.module;

    // A cell per `proto` or `absent` way: a site whose one such way is its
    // only way shares the cell every such site of the same place and shape
    // uses (ShapeCells::memberCellFor), else the site's own group.
    const auto count = static_cast<uint32_t>(plan.callees.size());
    auto kindOf = [](uint32_t place) {
        return place == il::Module::kNoMemberCallee ? il::Module::MemberKind::Absent : il::Module::MemberKind::Data;
    };
    std::vector<uint32_t> cellOf(count, UINT32_MAX);
    std::vector<uint32_t> members;
    for (uint32_t k = 0; k < count; ++k) {
        if (plan.callees[k] != kMemberDataOwn) members.push_back(k);
    }
    if (count == 1 && members.size() == 1) {
        cellOf[0] = ctx.cells.memberCellFor(kindOf(plan.callees[0]), site.keyIndex, plan.callees[0],
                                            plan.shapes[0].key);
    } else {
        const auto group = static_cast<uint32_t>(module.memberCells.size());
        for (uint32_t m = 0; m < members.size(); ++m) {
            il::Module::MemberCell cell;
            cell.kind = kindOf(plan.callees[members[m]]);
            cell.keyIndex = site.keyIndex;
            cell.callee = plan.callees[members[m]];
            cell.group = group;
            cell.groupSize = static_cast<uint32_t>(members.size());
            module.memberCells.push_back(cell);
            cellOf[members[m]] = group + m;
        }
    }
    std::vector<il::Instruction> publish;
    for (uint32_t k : members) {
        il::Instruction ask;
        ask.op = il::Op::MemberPublish;
        ask.type = il::Type::Void;
        ask.operands = {receiver};
        ask.immI32 = static_cast<int32_t>(cellOf[k]);
        publish.push_back(std::move(ask));
    }

    const ValueId shape = emitShapeOf(b, receiver);
    const BlockId join = b.newBlock();
    const BlockId exit = count <= kMaxChainShapes && !plan.calleesOpen ? b.exitBeforeWith(std::move(publish))
                                                                       : b.genericArm(join, std::move(publish));
    b.resultParam(join);

    // The test chain; an arm per place, made on first use. An own way's arm
    // is keyed by its slot (and representation), below every data place.
    struct Arm {
        BlockId block = il::kNoBlock;
        uint32_t way = 0;  // the first way that reaches it
    };
    std::map<uint64_t, Arm> arms;
    std::vector<uint64_t> armOrder;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t place = plan.callees[k];
        const uint64_t armKey = place == kMemberDataOwn
                                    ? (uint64_t{static_cast<uint32_t>(plan.shapes[k].slot)} << 1) |
                                          (plan.shapes[k].number ? 1u : 0u)
                                    : (uint64_t{1} << 40) | place;
        auto [it, fresh] = arms.emplace(armKey, Arm{});
        if (fresh) {
            it->second = Arm{b.newBlock(), k};
            armOrder.push_back(armKey);
        }
        const ValueId hit = place == kMemberDataOwn
                                ? emitShapeIs(b, shape, plan.shapes[k].key)
                                : b.emit(il::Op::PtrEq, il::Type::Bool,
                                         {shape, b.emit(il::Op::MemberCell, il::Type::Ptr, {},
                                                        static_cast<int32_t>(cellOf[k]))});
        const bool last = k + 1 == count;
        const BlockId next = last ? exit : b.newBlock();
        b.branch(hit, it->second.block, {}, next, {});
        ++ctx.stats.guards;
        if (!last) b.moveTo(next);
    }
    auto slotLoad = [&](ValueId object, int32_t slot, bool number) {
        il::Instruction load;
        load.op = il::Op::SlotLoad;
        load.type = il::Type::Dynamic;
        load.operands = {object};
        load.immI32 = slot;
        if (number) {
            load.boxType = il::Type::F64;
            ++ctx.stats.numberSlotReads;
        }
        return b.emitInst(std::move(load));
    };
    for (uint64_t armKey : armOrder) {
        const Arm& arm = arms.at(armKey);
        b.moveTo(arm.block);
        const uint32_t place = plan.callees[arm.way];
        ValueId r = il::kNoValue;
        if (place == kMemberDataOwn) {
            r = slotLoad(receiver, plan.shapes[arm.way].slot, plan.shapes[arm.way].number);
        } else if (place == il::Module::kNoMemberCallee) {
            r = b.constUndefined();
        } else {
            const uint32_t depth = place >> il::Module::kMemberDataSlotBits;
            const auto slot = static_cast<int32_t>(place & ((1u << il::Module::kMemberDataSlotBits) - 1));
            ValueId link = b.emit(il::Op::ShapeProto, il::Type::Dynamic, {shape});
            for (uint32_t d = 1; d < depth; ++d) {
                const ValueId linkShape = b.emit(il::Op::LoadShape, il::Type::Ptr, {link});
                link = b.emit(il::Op::ShapeProto, il::Type::Dynamic, {linkShape});
            }
            r = slotLoad(link, slot, false);
        }
        b.jump(join, {r});
    }
    b.moveTo(join);
    b.finish();
    ++ctx.stats.propReads;
    ++ctx.stats.propProtoReads;
}

}  // namespace bronze::opt::speculate

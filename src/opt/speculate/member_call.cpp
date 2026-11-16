// MEMBERS ABOVE THE RECEIVER: a method call, a getter read or a setter write
// whose member the harvest found on the receiver's prototype chain, one
// function per receiver shape (evidence_member.cpp planMember). Each shape the
// site held gets a MEMBER CELL (il.h Module::memberCells, one group per site,
// or for a site of one shape the cell every such site of the same member and
// shape shares) for its function, and the site becomes a shape test chain
// over the cells:
//
//     guard is.object %recv
//     s = load.shape %recv
//     br (ptr.eq s, (member.cell c1)), arm(f1), next1
//   next1:
//     br (ptr.eq s, (member.cell c2)), arm(f2), exit
//   arm(f):
//     r = call @entry(f)(%recv, args...)      ; direct_call.cpp
//     jump join(r)
//   exit [cold]:
//     member.publish %recv, c1 ; member.publish %recv, c2
//     leave @G, <point in front of the site>, state
//
// or, for a site of more cells than kMaxChainShapes, the publishing arm does
// the generic operation and jumps to `join` (the site stays in S).
//
// with `args` the method call's arguments, nothing for a getter and the
// assigned value for a setter; a setter's value is dropped (the assignment
// has no result). Cells whose functions are the same share one arm. An
// ABSENT read's cells all share the one arm `r = const.undefined`.
//
// Why exact: a cell holds a shape only while the runtime's check stands
// (runtime/accessor_cell.cpp): a plain receiver of that shape lacks the key
// as its own, and its chain resolves the key, at the thread's prototype
// epoch, to a data property holding a function with f's code (a method) or to
// an accessor whose getter or setter has f's code; every change that could
// make the chain answer otherwise bumps the epoch, and a bump empties the
// cell. So for such a receiver [[Get]] reads that function without running
// anything (or, for an accessor, calls it with the receiver as `this`), and
// OrdinarySet calls the setter with the receiver and the value. The direct
// call needs no function object: f reads no environment record
// (planMember refuses one that does), and its code binds `this` from the
// receiver as the generic dispatch would. Before the call nothing observable
// has run (a test, a header load, cell reads), so a receiver no cell holds
// leaves in front of the site and G performs the whole generic operation, or
// the cold arm performs it; either first asks the runtime to fill a cell for
// that receiver, which runs no user code. The calls carry the point after the site as their
// `resumeAfter`, so the inliner can fold the functions in.

#include <unordered_map>

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

void rewriteMember(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::MemberMethod || plan.kind == SiteKind::MemberGet ||
           plan.kind == SiteKind::MemberSet || plan.kind == SiteKind::MemberAbsent);
    assert(!plan.callees.empty() && plan.callees.size() <= kMaxSetShapes);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(!site.operands.empty());
    const ValueId receiver = site.operands[0];
    std::vector<ValueId> args;
    il::Module::MemberKind kind = il::Module::MemberKind::Method;
    switch (plan.kind) {
        case SiteKind::MemberMethod:
            assert(site.op == il::Op::MethodCall);
            args.assign(site.operands.begin() + 1, site.operands.end());
            break;
        case SiteKind::MemberGet:
            assert(site.op == il::Op::PropGet && site.operands.size() == 1);
            kind = il::Module::MemberKind::Getter;
            break;
        case SiteKind::MemberAbsent:
            assert(site.op == il::Op::PropGet && site.operands.size() == 1);
            kind = il::Module::MemberKind::Absent;
            break;
        default:
            assert(site.op == il::Op::PropSet && site.operands.size() == 2 && site.result == il::kNoValue);
            kind = il::Module::MemberKind::Setter;
            args.push_back(site.operands[1]);
            break;
    }

    // The site's group of cells, one per receiver shape the harvest held; a
    // site of one shape shares its cell (ShapeCells::memberCellFor).
    il::Module& module = ctx.module;
    const auto count = static_cast<uint32_t>(plan.callees.size());
    std::vector<uint32_t> cells;
    if (count == 1 && plan.shapes.size() == 1) {
        const uint32_t callee = plan.kind == SiteKind::MemberAbsent ? il::Module::kNoMemberCallee
                                                                    : ctx.entries.entryOf(plan.callees[0]);
        cells.push_back(ctx.cells.memberCellFor(kind, site.keyIndex, callee, plan.shapes[0].key));
    } else {
        const auto group = static_cast<uint32_t>(module.memberCells.size());
        for (uint32_t k = 0; k < count; ++k) {
            il::Module::MemberCell cell;
            cell.kind = kind;
            cell.keyIndex = site.keyIndex;
            cell.callee = plan.kind == SiteKind::MemberAbsent ? il::Module::kNoMemberCallee
                                                              : ctx.entries.entryOf(plan.callees[k]);
            cell.group = group;
            cell.groupSize = count;
            module.memberCells.push_back(cell);
            cells.push_back(group + k);
        }
    }
    std::vector<il::Instruction> publish;
    for (uint32_t c : cells) {
        il::Instruction ask;
        ask.op = il::Op::MemberPublish;
        ask.type = il::Type::Void;
        ask.operands = {receiver};
        ask.immI32 = static_cast<int32_t>(c);
        publish.push_back(std::move(ask));
    }

    // A miss asks the runtime to fill a cell for the receiver; then a site of
    // at most a chain's cells leaves, and one of more (its receivers came in
    // more shapes than a chain is planned for) does the generic operation in
    // a cold arm and stays in S: with every cell taken, a receiver of one
    // more shape would leave on every call. (A generic arm at the smaller
    // sites measured worse: its join erases what is known after the site.)
    const ValueId shape = emitShapeOf(b, receiver);
    const bool hasResult = site.result != il::kNoValue;
    const BlockId join = b.newBlock();
    const BlockId exit = count <= kMaxChainShapes ? b.exitBeforeWith(std::move(publish))
                                                  : b.genericArm(join, std::move(publish));
    if (hasResult) b.resultParam(join);

    // The test chain; an arm per distinct function, made on first use.
    std::unordered_map<uint32_t, BlockId> arms;
    std::vector<uint32_t> armOrder;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t f = plan.callees[k];
        auto [it, fresh] = arms.emplace(f, il::kNoBlock);
        if (fresh) {
            it->second = b.newBlock();
            armOrder.push_back(f);
        }
        const ValueId cellShape = b.emit(il::Op::MemberCell, il::Type::Ptr, {}, static_cast<int32_t>(cells[k]));
        const ValueId hit = b.emit(il::Op::PtrEq, il::Type::Bool, {shape, cellShape});
        const bool last = k + 1 == count;
        const BlockId next = last ? exit : b.newBlock();
        b.branch(hit, it->second, {}, next, {});
        ++ctx.stats.guards;
        if (!last) b.moveTo(next);
    }
    for (uint32_t f : armOrder) {
        b.moveTo(arms.at(f));
        const ValueId r = plan.kind == SiteKind::MemberAbsent ? b.constUndefined()
                                                              : emitDirectCallValue(b, f, il::kNoValue, receiver, args);
        b.jump(join, hasResult ? std::vector<ValueId>{r} : std::vector<ValueId>{});
    }
    b.moveTo(join);
    b.finish();
    switch (plan.kind) {
        case SiteKind::MemberMethod: ++ctx.stats.methodCalls; break;
        case SiteKind::MemberGet:
        case SiteKind::MemberAbsent: ++ctx.stats.propReads; break;
        default: ++ctx.stats.propWrites; break;
    }
}

}  // namespace bronze::opt::speculate

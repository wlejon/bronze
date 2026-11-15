// ELEMENT ACCESS: `r = elem.get %arr, %i` and `elem.set %arr, %i, %v` (an
// assignment) on a dense Array at an int32 index.
//
// Evidence (evidence.cpp planElem): the harvested build armed the site, its
// generic fallback never ran, its Array arm ran and its typed-array arm did
// not — every execution read or wrote an in-bounds, present element of an
// Array, which is everything the fallback did not cover. A recorded profile
// has no element evidence (no site observes the receiver).
//
// Read:
//     guard is.dense_array %arr, 0     ; an Array, element block, hazard clear
//     guard is.i32 %i ; x = unbox.i32.unchecked %i
//     n = array.length %arr
//     guard in.bounds x, n             ; unsigned: a negative index is out
//     r = elem.load %arr, x
//     guard !is.hole r
//
// Write — an existing element only:
//     guard is.plain_array %arr        ; bare array shape: no integrity level
//     guard is.dense_array %arr, 0
//     guard is.i32 %i ; x = ... ; n = ... ; guard in.bounds x, n
//     old = elem.load %arr, x
//     guard !is.hole old               ; the element exists
//     elem.store %arr, x, %v
//
// Why exact: an int32 Number index is the array index whose canonical string
// ToPropertyKey makes. `is.dense_array` is the backend's receiver proof (an
// Array whose element block answers its in-bounds reads with no user code:
// the array-index hazard is clear, so no element is an accessor and none is
// inherited through a hole), so an in-bounds slot that is not a hole is an
// own data element and [[Get]] answers it without asking the prototype. For
// the write, the same element is an own WRITABLE data property — the bare
// shape rules out every integrity level and the hazard any defined element —
// so OrdinarySet stores it in place: no setter, no length change, no growth.
// A hole or an index at or past the length is what the generic op handles
// differently (the prototype chain; growth), and both leave. Everything
// before the last guard is a test or a load, so a failure re-runs the
// generic instruction in G. Typed arrays stay generic (il_op.h: their loads
// need the view's offset and nine element kinds); so do writes that grow the
// array, holes, and defines.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

// The in-bounds int32 index, guarded.
ValueId guardIndex(GuardBuilder& b, ValueId array, ValueId index) {
    b.guard(b.emit(il::Op::IsI32, il::Type::Bool, {index}));
    const ValueId x = b.emit(il::Op::UnboxI32, il::Type::I32, {index});
    const ValueId length = b.emit(il::Op::ArrayLength, il::Type::I32, {array});
    b.guard(b.emit(il::Op::InBounds, il::Type::Bool, {x, length}));
    return x;
}

}  // namespace

void rewriteElem(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::ElemRead || plan.kind == SiteKind::ElemWrite);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    const ValueId array = site.operands[0];
    const ValueId index = site.operands[1];

    if (plan.kind == SiteKind::ElemRead) {
        assert(site.op == il::Op::ElemGet && site.type == il::Type::Dynamic);
        b.guard(b.emit(il::Op::IsDenseArray, il::Type::Bool, {array}, 0));
        const ValueId x = guardIndex(b, array, index);
        il::Instruction load;
        load.op = il::Op::ElemLoad;
        load.type = il::Type::Dynamic;
        load.operands = {array, x};
        b.emitResult(std::move(load));
        // The result is the hole only on the edge that leaves, where nothing
        // reads it: the exit resumes in front of the `elem.get`.
        b.guard(b.emit(il::Op::IsHole, il::Type::Bool, {site.result}), false);
        b.finish();
        ++ctx.stats.elementReads;
        return;
    }

    assert(site.op == il::Op::ElemSet && !il::isDefineWrite(site));
    const ValueId value = site.operands[2];
    b.guard(b.emit(il::Op::IsPlainArray, il::Type::Bool, {array}));
    b.guard(b.emit(il::Op::IsDenseArray, il::Type::Bool, {array}, 0));
    const ValueId x = guardIndex(b, array, index);
    const ValueId old = b.emit(il::Op::ElemLoad, il::Type::Dynamic, {array, x});
    b.guard(b.emit(il::Op::IsHole, il::Type::Bool, {old}), false);
    b.emit(il::Op::ElemStore, il::Type::Void, {array, x, value});
    b.finish();
    ++ctx.stats.elementWrites;
}

}  // namespace bronze::opt::speculate

// ELEMENT ACCESS: `r = elem.get %a, %i` and `elem.set %a, %i, %v` (an
// assignment), the same at a constant index (`r = prop.get %a, "k"` and
// `prop.set %a, "k", %v` with an index key k), on a dense Array or on a typed
// array of one element kind; and `r = prop.get %a, "length"` on an Array.
//
// Evidence (evidence.cpp planElem, planLength): the harvested build armed the
// site, its generic fallback never ran, and exactly one of its Array arm and
// its per-kind typed-array arms did (nor an append, nor a view index out of
// bounds) — every execution read or wrote an in-bounds, present element of
// that one receiver kind, which is everything the fallback did not cover. A
// recorded profile has no element evidence (no site observes the receiver).
//
// Dense read:
//     guard is.dense_array %a, 0       ; an Array, element block, hazard clear
//     guard is.i32 %i ; x = unbox.i32.unchecked %i
//     n = array.length %a
//     guard in.bounds x, n             ; unsigned: a negative index is out
//     r = elem.load %a, x
//     guard !is.hole r
// at a constant index k the test is `is.dense_array %a, K` with K the largest
// constant index planned for %a in the function (evidence.cpp
// shareConstBounds): it covers k < length, so there is no length read, and
// the first test of %a proves the rest.
//
// Dense write — an existing element only:
//     guard is.plain_array %a          ; bare array shape: no integrity level
//     guard is.dense_array %a, 0 (or K)
//     x, bounds as for the read
//     old = elem.load %a, x
//     guard !is.hole old               ; the element exists
//     elem.store %a, x, %v
//
// Typed read / write (one element kind: Float32, Float64, Int32):
//     guard is.typed_array %a, kind    ; the kind is fixed: a value fact
//     x as above (or const.i32 k)
//     n = typed.length %a
//     guard in.bounds x, n
//     v = typed.load %a, x, kind       ; f64 (float kinds) or i32 ; r = box v
//   or, a write:
//     guard is.number %v ; f = unbox.f64.unchecked %v ; (Int32: t = to.int32 f)
//     typed.store %a, x, f|t, kind
//
// Length:
//     guard is.array %a ; n = array.length %a ; guard n >= 0 ; r = box.i32 n
//
// Why exact: an int32 Number index is the array index whose canonical string
// ToPropertyKey makes, and an index key names the same element. For a dense
// Array, `is.dense_array` is the backend's receiver proof (an Array whose
// element block answers its in-bounds reads with no user code: the
// array-index hazard is clear, so no element is an accessor and none is
// inherited through a hole), so an in-bounds slot that is not a hole is an
// own data element and [[Get]] answers it without asking the prototype; for
// the write, the same element is an own WRITABLE data property (the bare shape
// rules out every integrity level and the hazard any defined element), so
// OrdinarySet stores it in place. A typed array's integer-indexed [[Get]] /
// [[Set]] (10.4.5) of a valid index is the element read / the element write of
// ToNumber(v) converted to the kind (a Number's ToNumber is itself; Int32's
// conversion is ToInt32), and an index below the view's length is valid. An
// Array's `length` is its own non-configurable data property, the length
// word. A hole, an index out of bounds, another receiver and a non-Number
// stored to a view are what the generic op handles differently, and all
// leave. Everything before the last guard is a test or a load, so a failure
// re-runs the generic instruction in G. Writes that grow the array, holes,
// defines, and the other typed element kinds stay generic.

#include "abi/bronze_abi.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

// The int32 index, unboxed under its guard, or the constant.
ValueId indexValue(GuardBuilder& b, const SitePlan& plan, ValueId index) {
    if (plan.constIndex != kNoConstIndex) return b.constI32(plan.constIndex);
    if (b.typeOf(index) == il::Type::I32) return index;
    b.guard(b.emit(il::Op::IsI32, il::Type::Bool, {index}));
    return b.emit(il::Op::UnboxI32, il::Type::I32, {index});
}

// A dense Array's in-bounds int32 index, guarded with the receiver.
ValueId guardDense(GuardBuilder& b, const SitePlan& plan, ValueId array, ValueId index) {
    const bool constant = plan.constIndex != kNoConstIndex;
    b.guard(b.emit(il::Op::IsDenseArray, il::Type::Bool, {array}, constant ? plan.constBound : 0));
    const ValueId x = indexValue(b, plan, index);
    if (!constant) {
        const ValueId length = b.emit(il::Op::ArrayLength, il::Type::I32, {array});
        b.guard(b.emit(il::Op::InBounds, il::Type::Bool, {x, length}));
    }
    return x;
}

// A typed array's in-bounds int32 index, guarded with the receiver's kind.
ValueId guardTyped(GuardBuilder& b, const SitePlan& plan, ValueId view, ValueId index) {
    b.guard(b.emit(il::Op::IsTypedArray, il::Type::Bool, {view}, plan.viewKind));
    const ValueId x = indexValue(b, plan, index);
    const ValueId length = b.emit(il::Op::TypedLength, il::Type::I32, {view});
    b.guard(b.emit(il::Op::InBounds, il::Type::Bool, {x, length}));
    return x;
}

il::Type typedValueType(int32_t kind) { return kind == BRONZE_ABI_TA_KIND_INT32 ? il::Type::I32 : il::Type::F64; }

}  // namespace

void rewriteElem(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::ElemRead || plan.kind == SiteKind::ElemWrite);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    const bool keyed = site.op == il::Op::ElemGet || site.op == il::Op::ElemSet;
    assert(keyed == (plan.constIndex == kNoConstIndex));
    const ValueId array = site.operands[0];
    const ValueId index = keyed ? site.operands[1] : il::kNoValue;
    const bool typed = plan.viewKind != kDenseArray;

    if (plan.kind == SiteKind::ElemRead) {
        assert(site.type == il::Type::Dynamic);
        if (typed) {
            const ValueId x = guardTyped(b, plan, array, index);
            const ValueId v =
                b.emit(il::Op::TypedLoad, typedValueType(plan.viewKind), {array, x}, plan.viewKind);
            il::Instruction boxed;
            boxed.op = il::Op::Box;
            boxed.type = il::Type::Dynamic;
            boxed.boxType = typedValueType(plan.viewKind);
            boxed.operands = {v};
            b.emitResult(std::move(boxed));
        } else {
            const ValueId x = guardDense(b, plan, array, index);
            il::Instruction load;
            load.op = il::Op::ElemLoad;
            load.type = il::Type::Dynamic;
            load.operands = {array, x};
            b.emitResult(std::move(load));
            // The result is the hole only on the edge that leaves, where
            // nothing reads it: the exit resumes in front of the site.
            b.guard(b.emit(il::Op::IsHole, il::Type::Bool, {site.result}), false);
        }
        b.finish();
        ++ctx.stats.elementReads;
        return;
    }

    assert(!il::isDefineWrite(site));
    const ValueId value = site.operands[keyed ? 2 : 1];
    if (typed) {
        const ValueId x = guardTyped(b, plan, array, index);
        b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {value}));
        ValueId stored = b.emit(il::Op::UnboxF64, il::Type::F64, {value});
        if (plan.viewKind == BRONZE_ABI_TA_KIND_INT32) {
            il::Instruction conv;
            conv.op = il::Op::ToInt32;
            conv.type = il::Type::I32;
            conv.boxType = il::Type::F64;
            conv.operands = {stored};
            stored = b.emitInst(std::move(conv));
        }
        b.emit(il::Op::TypedStore, il::Type::Void, {array, x, stored}, plan.viewKind);
    } else {
        b.guard(b.emit(il::Op::IsPlainArray, il::Type::Bool, {array}));
        const ValueId x = guardDense(b, plan, array, index);
        const ValueId old = b.emit(il::Op::ElemLoad, il::Type::Dynamic, {array, x});
        b.guard(b.emit(il::Op::IsHole, il::Type::Bool, {old}), false);
        b.emit(il::Op::ElemStore, il::Type::Void, {array, x, value});
    }
    b.finish();
    ++ctx.stats.elementWrites;
}

void rewriteArrayLength(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::ArrayLength);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(site.op == il::Op::PropGet && site.type == il::Type::Dynamic);
    const ValueId array = site.operands[0];
    b.guard(b.emit(il::Op::IsArray, il::Type::Bool, {array}));
    const ValueId n = b.emit(il::Op::ArrayLength, il::Type::I32, {array});
    // The word is a uint32: a length past 2^31 - 1 is a Number no i32 holds.
    b.guard(b.emit(il::Op::CmpGe, il::Type::Bool, {n, b.constI32(0)}));
    il::Instruction boxed;
    boxed.op = il::Op::Box;
    boxed.type = il::Type::Dynamic;
    boxed.boxType = il::Type::I32;
    boxed.operands = {n};
    b.emitResult(std::move(boxed));
    b.finish();
    ++ctx.stats.arrayLengths;
}

}  // namespace bronze::opt::speculate

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
// Dense write — an element below the length, a hole included:
//     guard is.plain_array %a          ; bare array shape: no integrity level
//     guard is.dense_array %a, 0 (or K)
//     x, bounds as for the read
//     elem.store %a, x, %v
//
// Typed read / write (one element kind with a code, il2mir il_abi.h
// kElemViewKinds: every kind but Int8, Float16 and the BigInt kinds):
//     guard is.typed_array %a, kind    ; the kind is fixed: a value fact
//     x as above (or const.i32 k)
//     n = typed.length %a
//     guard in.bounds x, n
//     v = typed.load %a, x, kind       ; f64 (float kinds, Uint32) or i32 ; r = box v
//   or, a write:
//     guard is.number %v ; f = unbox.f64.unchecked %v
//     (an integer kind but Uint8Clamped: t = to.int32 f)
//     typed.store %a, x, f|t, kind     ; the low bits, or f rounded (Float32)
//                                      ; or clamped (Uint8Clamped)
// Two kinds (three.js's slerpFlat and PropertyMixer meet Float32Array tracks
// and Float64Array buffers at one site): the index (and a write's value)
// first, then `br is.typed_array %a, kind1` to that kind's bounds test and
// access and a guard of the second kind for the other's, the two meeting at a
// join; a read's join takes the loaded value, which both kinds must load as
// one type, and boxes it once.
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
// OrdinarySet stores it in place; and a hole below the length is no own
// property and, with the hazard clear, no prototype's either, so OrdinarySet
// reaches CreateDataProperty on the receiver, which an array at the bare
// shape accepts at any index (runtime/integrity.h
// rtArrayElementWriteRefusal): the element is stored where the hole was and
// the length, already past it, stays. A typed array's integer-indexed [[Get]] /
// [[Set]] (10.4.5) of a valid index is the element read / the element write of
// ToNumber(v) converted to the kind (a Number's ToNumber is itself; an
// integer kind's conversion keeps the low bits of ToInt32, Uint8Clamped's is
// ToUint8Clamp), and an index below the view's length is valid. An
// Array's `length` is its own non-configurable data property, the length
// word. A hole read, an index out of bounds, another receiver and a
// non-Number stored to a view are what the generic op handles differently,
// and all leave. Everything before the last guard is a test or a load, so a
// failure re-runs the generic instruction in G. Writes that grow the array,
// defines, and the typed element kinds with no code stay generic.

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
    const int32_t numbers = plan.kind == SiteKind::ElemRead && plan.numbers ? il::kDenseNumbers : 0;
    b.guard(b.emit(il::Op::IsDenseArray, il::Type::Bool, {array}, (constant ? plan.constBound : 0) | numbers));
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

// The access at the cursor for each kind of a typed site: `access(kind)`
// runs on the receiver of that kind with the index in bounds and answers the
// value to join (kNoValue for a write). One kind is guarded; of two, the first
// is a branch and the second a guard. Answers the joined value of `type`.
template <typename Access>
ValueId perKind(GuardBuilder& b, const SitePlan& plan, ValueId view, ValueId x, il::Type type, Access access) {
    const std::vector<int32_t> kinds = plan.otherViewKind == kDenseArray
                                           ? std::vector<int32_t>{plan.viewKind}
                                           : std::vector<int32_t>{plan.viewKind, plan.otherViewKind};
    const BlockId join = b.newBlock();
    const ValueId joined = type == il::Type::Void ? il::kNoValue : b.addParam(join, type);
    for (size_t i = 0; i < kinds.size(); ++i) {
        const ValueId is = b.emit(il::Op::IsTypedArray, il::Type::Bool, {view}, kinds[i]);
        BlockId next = il::kNoBlock;
        if (i + 1 < kinds.size()) {
            const BlockId hit = b.newBlock();
            next = b.newBlock();
            b.branch(is, hit, {}, next, {});
            b.moveTo(hit);
        } else {
            b.guard(is);
        }
        const ValueId length = b.emit(il::Op::TypedLength, il::Type::I32, {view});
        b.guard(b.emit(il::Op::InBounds, il::Type::Bool, {x, length}));
        const ValueId v = access(kinds[i]);
        b.jump(join, type == il::Type::Void ? std::vector<ValueId>{} : std::vector<ValueId>{v});
        if (next != il::kNoBlock) b.moveTo(next);
    }
    b.moveTo(join);
    return joined;
}

// typed.load's result (il_op.h): f64 for the float kinds and Uint32, whose
// elements an i32 does not hold, i32 for the other integer kinds.
il::Type typedValueType(int32_t kind) {
    return kind == BRONZE_ABI_TA_KIND_FLOAT32 || kind == BRONZE_ABI_TA_KIND_FLOAT64 || kind == BRONZE_ABI_TA_KIND_UINT32
               ? il::Type::F64
               : il::Type::I32;
}

// What typed.store takes for the kind: the double for the float kinds and
// Uint8Clamped (whose rounding the store does), the ToInt32 of it for the
// integer kinds, whose low bits are ToInt16's, ToUint8's, ... (7.1.6-7.1.11:
// each is the same modular reduction to fewer bits).
bool storesDouble(int32_t kind) {
    return kind == BRONZE_ABI_TA_KIND_FLOAT32 || kind == BRONZE_ABI_TA_KIND_FLOAT64 ||
           kind == BRONZE_ABI_TA_KIND_UINT8CLAMPED;
}

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
            const il::Type type = typedValueType(plan.viewKind);
            ValueId v = il::kNoValue;
            if (plan.otherViewKind == kDenseArray) {
                const ValueId x = guardTyped(b, plan, array, index);
                v = b.emit(il::Op::TypedLoad, type, {array, x}, plan.viewKind);
            } else {
                const ValueId x = indexValue(b, plan, index);
                v = perKind(b, plan, array, x, type, [&](int32_t kind) {
                    return b.emit(il::Op::TypedLoad, type, {array, x}, kind);
                });
            }
            il::Instruction boxed;
            boxed.op = il::Op::Box;
            boxed.type = il::Type::Dynamic;
            boxed.boxType = type;
            boxed.operands = {v};
            b.emitResult(std::move(boxed));
        } else {
            const ValueId x = guardDense(b, plan, array, index);
            il::Instruction load;
            load.op = il::Op::ElemLoad;
            load.type = il::Type::Dynamic;
            load.operands = {array, x};
            load.immI32 = plan.numbers ? il::kElemLoadNumber : 0;
            b.emitResult(std::move(load));
            // The result is the hole only on the edge that leaves, where
            // nothing reads it: the exit resumes in front of the site. A
            // NUMBERS array has no hole below its length.
            if (!plan.numbers) b.guard(b.emit(il::Op::IsHole, il::Type::Bool, {site.result}), false);
        }
        b.finish();
        ++ctx.stats.elementReads;
        return;
    }

    assert(!il::isDefineWrite(site));
    const ValueId value = site.operands[keyed ? 2 : 1];
    if (typed) {
        // The double, or for an integer kind but Uint8Clamped its ToInt32.
        const auto store = [&](ValueId x, ValueId f, int32_t kind) {
            ValueId stored = f;
            if (!storesDouble(kind)) {
                il::Instruction conv;
                conv.op = il::Op::ToInt32;
                conv.type = il::Type::I32;
                conv.boxType = il::Type::F64;
                conv.operands = {f};
                stored = b.emitInst(std::move(conv));
            }
            b.emit(il::Op::TypedStore, il::Type::Void, {array, x, stored}, kind);
            return il::kNoValue;
        };
        if (plan.otherViewKind == kDenseArray) {
            const ValueId x = guardTyped(b, plan, array, index);
            b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {value}));
            store(x, b.emit(il::Op::UnboxF64, il::Type::F64, {value}), plan.viewKind);
        } else {
            const ValueId x = indexValue(b, plan, index);
            b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {value}));
            const ValueId f = b.emit(il::Op::UnboxF64, il::Type::F64, {value});
            perKind(b, plan, array, x, il::Type::Void, [&](int32_t kind) { return store(x, f, kind); });
        }
    } else if (plan.grow) {
        b.guard(b.emit(il::Op::IsPlainArray, il::Type::Bool, {array}));
        const ValueId x = indexValue(b, plan, index);
        const ValueId n = b.emit(il::Op::ArrayLength, il::Type::I32, {array});
        b.guard(b.emit(il::Op::CmpGe, il::Type::Bool, {x, b.constI32(0)}));
        b.guard(b.emit(il::Op::CmpLe, il::Type::Bool, {x, n}));
        b.emit(il::Op::ElemStore, il::Type::Void, {array, x, value}, il::kStoreGrow);
    } else {
        b.guard(b.emit(il::Op::IsPlainArray, il::Type::Bool, {array}));
        const ValueId x = guardDense(b, plan, array, index);
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

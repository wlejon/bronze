// `o[k] = v` with a computed key, on a receiver whose class layout is proven,
// split on the key into the named stores it may be.
//
// WHY. three.js's `BoxGeometry.buildPlane` writes a `Vector3` through string
// parameters — `vector[ u ] = x * udir` with `u` one of 'x', 'y', 'z' — six
// such stores a vertex, 144 a box. A computed store is the runtime's
// `bronze_elem_set`: a hash of the key, a probe of the per-thread store cache
// that compares the key's contents, then `setProp` through that entry — about
// 230 instructions, where the named store `vector.x = ...` is a shape compare
// and a slot write. The receiver's class says which names can be fields; the
// key is the only unknown.
//
// WHAT IT EMITS. For each field F of the receiver's layout (at most
// kMaxFields), in layout order:
//
//     if (k === "F") o.F = v  (the prop.set `o["F"] = v` would be, its static
//                              slot claim and inline cache included)
//
// and, when no field matched, the `elem.set o, k, v` the site always had.
// The `===` is marked kStrictEqIdentityDispatch (il.h): both of its edges are
// correct for a key that is "F" at another address (the named store and the
// computed one are the same program for it), so the backend compares bits —
// the key constants are the interned headers a literal key evaluates to — and
// a String it cannot match that way takes the elem.set, as before.
//
// WHY IT IS THE SAME PROGRAM. By the time the store runs, `o`, `k` and `v` are
// evaluated, in source order, by the caller. `k === "F"` is true exactly when
// `k` is the String "F" (7.2.15: a String is strictly equal only to a String of
// the same code units), it reads nothing and runs nothing, and for that `k`
// ToPropertyKey (7.1.19) is "F" itself, so `o[k] = v` IS `o.F = v` (13.15.2,
// PutValue with the same reference). Every other `k` — another string, a
// number, a symbol, an object whose toString would run — fails every compare
// and reaches the elem.set unchanged, which does ToPropertyKey as before. The
// named store keeps every guard a named store has: a receiver whose shape is
// not the layout's takes its inline cache, never a wrong slot.
//
// WHAT IT REFUSES. A compound or logical assignment (their read half is an
// elem.get, left alone); a key inference proved is not a String; a receiver
// without a proven layout, with more than kMaxFields fields, or with a field
// claimStaticSlot will not claim for a write; and any program compiled with
// --pins, whose field barriers a named store would have to run where the
// computed store never did.
//
// THE SEAM is `BRONZE_NO_KEYED_FIELD_STORE=1`, read by the compiler once per
// process.

#include <cstdlib>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

// Each field is a compare and a store at every such site, so the split is for
// small records (vectors, colours, quaternions), not for a class with dozens of
// fields where a key would pay a long chain of compares before its store.
constexpr size_t kMaxFields = 4;

bool keyedFieldStoreDisabled() {
    static const bool off = std::getenv("BRONZE_NO_KEYED_FIELD_STORE") != nullptr;
    return off;
}

}  // namespace

bool Lowerer::emitKeyedFieldStore(const ast::IndexAccess& idxAccess, Value objBoxed, Value keyBoxed,
                                  Value storedBoxed, il::Function& ilFn) {
    if (keyedFieldStoreDisabled() || inference_ == nullptr || pins_ != nullptr) return false;
    const types::Type keyType = inferredType(*idxAccess.index);
    if (!keyType.is(types::TypeKind::Dynamic) && !keyType.is(types::TypeKind::String)) return false;
    const types::Type recvType = inferredType(*idxAccess.object);
    if (!recvType.is(types::TypeKind::Object)) return false;
    const types::ClassLayout* layout = inference_->classLayouts.byShapeClass(recvType.shapeClass());
    if (layout == nullptr || !layout->layoutProven || layout->fields.empty() ||
        layout->fields.size() > kMaxFields) {
        return false;
    }
    for (const std::string& field : layout->fields) {
        if (!claimStaticSlot(*idxAccess.object, field, /*forWrite=*/true)) return false;
    }

    const il::BlockId join = createBlock(ilFn);
    for (const std::string& field : layout->fields) {
        const uint32_t keyIndex = getKeyConstantIndex(field);
        il::Instruction str;
        str.op = il::Op::Box;
        str.type = il::Type::Dynamic;
        str.boxType = il::Type::Str;
        str.result = ilFn.valueCount++;
        str.keyIndex = keyIndex;
        emitInst(ilFn, str);

        il::Instruction eq;
        eq.op = il::Op::StrictEq;
        eq.type = il::Type::Bool;
        eq.result = ilFn.valueCount++;
        eq.operands = {keyBoxed.id, str.result};
        eq.immI32 = il::Instruction::kStrictEqIdentityDispatch;
        emitInst(ilFn, eq);

        const il::BlockId store = createBlock(ilFn);
        const il::BlockId next = createBlock(ilFn);
        il::Instruction br;
        br.op = il::Op::Branch;
        br.type = il::Type::Void;
        br.result = il::kNoValue;
        br.operands = {eq.result};
        br.target = il::BlockTarget{.block = store, .args = {}};
        br.elseTarget = il::BlockTarget{.block = next, .args = {}};
        emitInst(ilFn, br);

        // The store `o["F"] = v` lowers to (lower_expr_assign.cpp's literal key).
        setCurrentBlock(store);
        il::Instruction set;
        set.op = il::Op::PropSet;
        set.type = il::Type::Void;
        set.result = il::kNoValue;
        set.operands = {objBoxed.id, storedBoxed.id};
        set.keyIndex = keyIndex;
        set.icIndex = icSiteCounter_++;
        set.icMonomorphic = monomorphicPropSite(*idxAccess.object);
        set.immI32 = strictFlag();
        stampStaticSlot(set, *idxAccess.object);
        emitInst(ilFn, set);
        il::Instruction jmp;
        jmp.op = il::Op::Jump;
        jmp.type = il::Type::Void;
        jmp.result = il::kNoValue;
        jmp.target = il::BlockTarget{.block = join, .args = {}};
        emitInst(ilFn, jmp);

        setCurrentBlock(next);
    }

    il::Instruction set;
    set.op = il::Op::ElemSet;
    set.type = il::Type::Void;
    set.result = il::kNoValue;
    set.operands = {objBoxed.id, keyBoxed.id, storedBoxed.id};
    set.immI32 = strictFlag();
    emitInst(ilFn, set);
    il::Instruction jmp;
    jmp.op = il::Op::Jump;
    jmp.type = il::Type::Void;
    jmp.result = il::kNoValue;
    jmp.target = il::BlockTarget{.block = join, .args = {}};
    emitInst(ilFn, jmp);

    setCurrentBlock(join);
    return true;
}

}  // namespace bronze::lower

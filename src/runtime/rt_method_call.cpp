#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/native_fn_memo.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/tls_block.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The EXOTIC-receiver latch: an Array, a collection (Map/Set/WeakMap/WeakSet)
// or a typed-array view, whose method is a native builtin from an immutable C
// table. The entry is the
// DIRECT form under a kind guard instead of a shape guard — bronze_abi.h's
// method-site contract states the word 0 encoding and why the box-offset
// clause is the complete shadowing story. What makes each latch sound:
//
//   Array: the probe's way 0 holds the ARRAY-METHOD sentinel, which the read
//   path fills only when the answer came from the method table, the receiver
//   carried no named-property box, and the array-method get IC is enabled
//   (rt_prop.cpp) — and that table cannot change, because decorating
//   `Array.prototype` is a hard error by construction (rt_prop_write.cpp).
//
//   Collections: the native-member memo (runtime/native_fn_memo.h) answers
//   (kind, key) with the interned ladder native and nothing else — a fill is
//   refused for anything that is not one — so `memo == callee` proves the
//   read answered from the C ladder rather than from an own property or a
//   subclass prototype, both of which live in the receiver's box and are
//   re-guarded per hit anyway. A memo that has not filled (or is disabled by
//   BRONZE_NO_FN_SINGLETON_CACHE) simply never latches here.
//
// Loads, compares and stores only — no allocation, like the caller.
void latchExoticMethodIc(uint64_t* icEntry, const HeapObjectHeader* objHdr, Value fnVal,
                         const FunctionHeader* fn, const InlineCache& probe, uint32_t keyIndex) {
    if (!rtExoticMethodIcEnabled()) return;
    // The original env rule, verbatim: only a callee that can be called with
    // undefined may have its env omitted from the entry. Every latchable
    // native satisfies it (rtNativeFunction creates env-free), but the latch
    // asks the object rather than trusting the table's provenance.
    if (fn->needsEnv() && !fn->env_record.isUndefined() &&
        fn->env_record.rawBits() != fnVal.rawBits()) {
        return;
    }
    const uint16_t kind = objHdr->flags;
    uint64_t boxOffset = 0;
    if (kind == HeapKind::Array) {
        if (!probe.isArrayMethod()) return;
        boxOffset = offsetof(ArrayHeader, properties);
    } else if (kind == MapHeader::kMapFlags || kind == MapHeader::kSetFlags ||
               kind == MapHeader::kWeakMapFlags || kind == MapHeader::kWeakSetFlags) {
        const Value memo = rtNativeMemberProbe(kind, keyIndex);
        if (memo.isUndefined() || memo.rawBits() != fnVal.rawBits()) return;
        boxOffset = offsetof(MapHeader, properties);
    } else if (kind == TypedArrayHeader::kFlags) {
        // Memo-keyed like the collections (the fill is gated on the shared
        // method table, rt_prop.cpp's typed-array branch). A view carries no
        // named-property box AT ALL, so no shadowing channel exists and the
        // guard's box clause only needs a word that can never read as
        // Object-tagged: the {byteOffset, length} word, whose top 16 bits
        // stay far below a pointer tag by construction (typed_array.h keeps
        // that word scan-safe for the collector, which is the same property).
        const Value memo = rtNativeMemberProbe(kind, keyIndex);
        if (memo.isUndefined() || memo.rawBits() != fnVal.rawBits()) return;
        boxOffset = offsetof(TypedArrayHeader, byteOffset);
    } else {
        return;
    }
    icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] = reinterpret_cast<uint64_t>(fn->code);
    icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD] = static_cast<uint64_t>(fn->arity);
    icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD] = BRONZE_ABI_UNDEFINED_BITS;
    icEntry[0] = (boxOffset << BRONZE_ABI_METHOD_IC_BOX_SHIFT) |
                 (static_cast<uint64_t>(kind) << BRONZE_ABI_METHOD_IC_KIND_SHIFT) |
                 BRONZE_ABI_METHOD_IC_EXOTIC_BIT;
}

// One latch for both method helpers. `probe` is the scratch site the property
// read just filled — a stack InlineCacheSite whose way 0, when it names the
// live receiver's shape as a plain DATA slot, says exactly where the method
// lives; every other outcome (accessor, absent, dictionary receiver, a ladder
// answer that fills nothing) leaves it unusable and the latch falls back to
// the original env-free rule — except the ARRAY-METHOD sentinel, which is the
// exotic latch's Array key above. `keyIndex` is the process-wide interned key,
// which only the exotic latch reads (the memo is keyed on it).
//
// The four installable forms, and why each is sound, are the ABI header's
// METHOD-CALL site contract (bronze_abi.h); the short version:
//   depth 0  -> SLOT form: cache only the slot index. Same-shape receivers
//               can hold different functions there (host functions,
//               per-instance closures), so the callee is re-derived from the
//               receiver at every hit and nothing about it is cached.
//   depth >= 1 -> DIRECT form, env included when the callee carries one: the
//               receiver's shape determines the holder chain, so the function
//               object — and therefore its environment record, written once
//               at creation — is as shape-stable as the code pointer the
//               original mechanism already cached.
//   non-Plain receiver -> EXOTIC form, latchExoticMethodIc above.
// The env word is GC-safe because the module registered it as a value cell
// at init (bronze_register_method_ic_cells): the collector forwards it in
// place, so the cell reads current bits after every flip.
//
// GC discipline: called after the property read, so `thisVal`/`fnVal` must be
// the RE-DERIVED values from the caller's roots — the read may have collected
// — and everything here is loads and stores, never an allocation.
void latchMethodIc(uint64_t* icEntry, Value thisVal, Value fnVal, const InlineCache& probe,
                   uint32_t keyIndex) {
    if (!icEntry || rtTls()->method_call_ic_enabled == 0) return;
    if (!thisVal.isObject() || !fnVal.isObject()) return;
    auto* objHdr = thisVal.asObject<HeapObjectHeader>();
    auto* fnHdr = fnVal.asObject<HeapObjectHeader>();
    if (fnHdr->flags != HeapKind::Function) return;
    auto* fn = reinterpret_cast<FunctionHeader*>(fnHdr);
    if (!fn->code) return;
    if (objHdr->flags != HeapKind::Plain) {
        // A non-Plain receiver has no shape word to guard on, so nothing below
        // this line may run for one — the exotic latch either installs its own
        // kind-guarded form or leaves the entry exactly as it was.
        latchExoticMethodIc(icEntry, objHdr, fnVal, fn, probe, keyIndex);
        return;
    }
    auto* obj = reinterpret_cast<ObjectHeader*>(objHdr);

    const uint64_t shapeWord = reinterpret_cast<uint64_t>(obj->shape);

    // The shape re-check against the PROBE guards a read that ran an accessor
    // or otherwise moved the world: a fill that describes some earlier shape
    // of this receiver must not be latched against its current one.
    if (rtEnvMethodIcEnabled() && probe.isRealShape() && !probe.isAccessor() &&
        !probe.isAbsent() && probe.cached_shape == obj->shape) {
        if (probe.realDepth() == 0) {
            icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] = 0;
            icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD] =
                (static_cast<uint64_t>(probe.cached_slot) + 1)
                << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
            icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD] = BRONZE_ABI_UNDEFINED_BITS;
            icEntry[0] = shapeWord;
            return;
        }
        if (fn->needsEnv() && !fn->env_record.isUndefined() &&
            fn->env_record.rawBits() != fnVal.rawBits()) {
            icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] = reinterpret_cast<uint64_t>(fn->code);
            icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD] = static_cast<uint64_t>(fn->arity);
            icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD] = fn->env_record.rawBits();
            icEntry[0] = shapeWord;
            return;
        }
        // An env-free callee on the chain falls through to the original rule
        // below, which it satisfies — the split exists only for the comment.
    }

    // The original rule: an env-free callee (or one whose env is itself — the
    // self-reference bronze_create_function writes for a capture-free
    // function, which its code never reads) may be called with undefined.
    if (!fn->needsEnv() || fn->env_record.isUndefined() ||
        fn->env_record.rawBits() == fnVal.rawBits()) {
        icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] = reinterpret_cast<uint64_t>(fn->code);
        icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD] = static_cast<uint64_t>(fn->arity);
        icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD] = BRONZE_ABI_UNDEFINED_BITS;
        icEntry[0] = shapeWord;
    }
}

}  // namespace

extern "C" {

uint64_t bronze_call_method(uint64_t thisBits, uint32_t keyIndex, uint32_t argc,
                            const uint64_t* argvBits, uint64_t* icEntry) {
    Rooted<Value> thisRoot{Value(thisBits)};
    // A stack site, not the module's entry: the property machinery installs
    // shape/slot/depth facts here without the module's method-site words —
    // which mean different things (bronze_abi.h) — ever aliasing a property
    // entry. It holds a non-moving Shape* and integers only, so it needs no
    // rooting.
    InlineCacheSite scratch{};
    uint64_t fnBits =
        bronze_prop_get(thisBits, keyIndex, reinterpret_cast<uint64_t*>(&scratch));
    if (rtExceptionPending()) return BRONZE_ABI_UNDEFINED_BITS;

    recordCallSite("bronze_call_method", fnBits);

    Rooted<Value> fnRoot{Value(fnBits)};
    latchMethodIc(icEntry, thisRoot.get(), fnRoot.get(), scratch.ways[0], keyIndex);

    return bronze_dynamic_call(fnRoot.get().rawBits(), thisRoot.get().rawBits(), argc, argvBits);
}

uint64_t bronze_call_method_spread(uint64_t thisBits, uint32_t keyIndex, uint64_t argsArrBits,
                                   uint64_t* icEntry) {
    Rooted<Value> thisRoot{Value(thisBits)};
    Rooted<Value> argsRoot{Value(argsArrBits)};
    InlineCacheSite scratch{};
    uint64_t fnBits =
        bronze_prop_get(thisBits, keyIndex, reinterpret_cast<uint64_t*>(&scratch));
    if (rtExceptionPending()) return BRONZE_ABI_UNDEFINED_BITS;

    recordCallSite("bronze_call_method_spread", fnBits);

    Rooted<Value> fnRoot{Value(fnBits)};
    latchMethodIc(icEntry, thisRoot.get(), fnRoot.get(), scratch.ways[0], keyIndex);

    return bronze_dynamic_call_spread(fnRoot.get().rawBits(), thisRoot.get().rawBits(),
                                      argsRoot.get().rawBits());
}

}  // extern "C"

}  // namespace bronze::runtime

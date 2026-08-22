#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// One latch for both method helpers. `probe` is the scratch site the property
// read just filled — a stack InlineCacheSite whose way 0, when it names the
// live receiver's shape as a plain DATA slot, says exactly where the method
// lives; every other outcome (accessor, absent, array-method sentinel,
// dictionary or exotic receiver, a ladder answer that fills nothing) leaves
// it unusable and the latch falls back to the original env-free rule.
//
// The three installable forms, and why each is sound, are the ABI header's
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
// The env word is GC-safe because the module registered it as a value cell
// at init (bronze_register_method_ic_cells): the collector forwards it in
// place, so the cell reads current bits after every flip.
//
// GC discipline: called after the property read, so `thisVal`/`fnVal` must be
// the RE-DERIVED values from the caller's roots — the read may have collected
// — and everything here is loads and stores, never an allocation.
void latchMethodIc(uint64_t* icEntry, Value thisVal, Value fnVal, const InlineCache& probe) {
    if (!icEntry || rtTls()->method_call_ic_enabled == 0) return;
    if (!thisVal.isObject() || !fnVal.isObject()) return;
    auto* objHdr = thisVal.asObject<HeapObjectHeader>();
    auto* fnHdr = fnVal.asObject<HeapObjectHeader>();
    if (objHdr->flags != HeapKind::Plain || fnHdr->flags != HeapKind::Function) return;
    auto* obj = reinterpret_cast<ObjectHeader*>(objHdr);
    auto* fn = reinterpret_cast<FunctionHeader*>(fnHdr);
    if (!fn->code) return;

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
    latchMethodIc(icEntry, thisRoot.get(), fnRoot.get(), scratch.ways[0]);

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
    latchMethodIc(icEntry, thisRoot.get(), fnRoot.get(), scratch.ways[0]);

    return bronze_dynamic_call_spread(fnRoot.get().rawBits(), thisRoot.get().rawBits(),
                                      argsRoot.get().rawBits());
}

}  // extern "C"

}  // namespace bronze::runtime

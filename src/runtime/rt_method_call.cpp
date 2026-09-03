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
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape_census.h"
#include "runtime/tls_block.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Way-1 displacement (the site contract's WAY 1 section, bronze_abi.h):
// before way 0 is overwritten for a DIFFERENT guard, a healthy PLAIN-receiver
// DIRECT resident is copied into words 6-9, so the two shapes of a
// polymorphic site — three.js's recursive `updateMatrixWorld` over a tree of
// Object3D/Mesh/Scene — each keep an entry instead of relatching forever.
// Only the direct form moves: a SLOT entry's code word is 0 and its word-2
// high half is its slot machinery, and an EXOTIC entry's word 0 carries the
// sentinel bit — both are refused, so way 1 can only ever hold what the
// generated way-1 arm knows how to dispatch. The env copy in word 9 is a
// registered value cell exactly as word 3 is (bronze_register_method_ic_cells
// registers both), which is what keeps the copy current across a flip.
void displaceMethodWay0(uint64_t* icEntry, uint64_t newWord0) {
    if (!rtPolyMethodIcEnabled()) return;
    const uint64_t w0 = icEntry[0];
    if (w0 == 0 || w0 == newWord0) return;
    if (w0 & BRONZE_ABI_METHOD_IC_EXOTIC_BIT) return;
    const uint64_t arityWord = icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD];
    if ((arityWord >> BRONZE_ABI_METHOD_IC_SLOT_SHIFT) != 0) return;
    if (icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] == 0) return;
    icEntry[BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD] = icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD];
    icEntry[BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD] = arityWord;
    icEntry[BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD] = icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD];
    icEntry[BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD] = w0;
}

// The EXOTIC-receiver latch: an Array, a collection (Map/Set/WeakMap/WeakSet),
// a typed-array view, or a global-constructor function receiver, whose method
// is a native builtin from an immutable C table. The entry is the
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
// Loads, compares and stores only — no allocation, like the caller. Answers
// whether it installed, so a Function receiver it declines can be offered to
// the statics latch below.
// The PRIMITIVE-receiver latch (bronze_abi.h's method-site contract, the
// primitive form): a STRING `this` whose method was read off
// `String.prototype`'s own slots. The probe is the scratch site the read
// filled, and it is what proves where the answer came from — its shape is
// the intrinsic's, at depth 0, a data slot, and that slot holds the callee.
// The entry is the SLOT form against the INTRINSIC: the env word names the
// holder (it is a collector-forwarded cell, and a slot hit never reads it as
// an env), the aux word pins the holder's shape, and the hit reads the
// slot's live value — so `String.prototype.charCodeAt = f`, which overwrites
// a slot without moving the shape, is seen on the next call. Loads, compares
// and stores only: the probe being real means the intrinsic was built by the
// walk that filled it, so `rtStringPrototype` here is a fetch and not a build.
bool latchPrimitiveMethodIc(uint64_t* icEntry, Value thisVal, Value fnVal,
                            const InlineCache& probe) {
    if (!rtExoticMethodIcEnabled()) return false;
    if (!thisVal.isString()) return false;
    if (!probe.isRealShape() || probe.isAccessor() || probe.isAbsent() || probe.realDepth() != 0) {
        return false;
    }
    if (fnVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return false;
    const Value proto = rtStringPrototype();
    if (!proto.isObject()) return false;
    const auto* holder = proto.asObject<ObjectHeader>();
    if (probe.cached_shape != holder->shape) return false;
    if (holder->getSlot(probe.cached_slot).rawBits() != fnVal.rawBits()) return false;
    const uint64_t newWord0 =
        (static_cast<uint64_t>(BRONZE_ABI_TAG_STRING) << BRONZE_ABI_METHOD_IC_KIND_SHIFT) |
        BRONZE_ABI_METHOD_IC_EXOTIC_BIT;
    displaceMethodWay0(icEntry, newWord0);
    icEntry[BRONZE_ABI_METHOD_IC_AUX_WORD] = reinterpret_cast<uint64_t>(holder->shape);
    icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] = 0;
    icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD] =
        (static_cast<uint64_t>(probe.cached_slot) + 1) << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
    icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD] = proto.rawBits();
    icEntry[0] = newWord0;
    return true;
}

bool latchExoticMethodIc(uint64_t* icEntry, const HeapObjectHeader* objHdr, Value fnVal,
                         const FunctionHeader* fn, const InlineCache& probe, uint32_t keyIndex) {
    if (!rtExoticMethodIcEnabled()) return false;
    // The original env rule, verbatim: only a callee that can be called with
    // undefined may have its env omitted from the entry. Every latchable
    // native satisfies it (rtNativeFunction creates env-free), but the latch
    // asks the object rather than trusting the table's provenance.
    if (fn->needsEnv() && !fn->env_record.isUndefined() &&
        fn->env_record.rawBits() != fnVal.rawBits()) {
        return false;
    }
    const uint16_t kind = objHdr->flags;
    uint64_t auxOffset = 0;
    uint64_t guardBits = 0;
    if (kind == HeapKind::Array) {
        if (!probe.isArrayMethod()) return false;
        auxOffset = offsetof(ArrayHeader, properties);
    } else if (kind == MapHeader::kMapFlags || kind == MapHeader::kSetFlags ||
               kind == MapHeader::kWeakMapFlags || kind == MapHeader::kWeakSetFlags) {
        const Value memo = rtNativeMemberProbe(kind, keyIndex);
        if (memo.isUndefined() || memo.rawBits() != fnVal.rawBits()) return false;
        auxOffset = offsetof(MapHeader, properties);
    } else if (kind == TypedArrayHeader::kFlags) {
        // Memo-keyed like the collections (the fill is gated on the shared
        // method table, rt_prop.cpp's typed-array branch). A view carries no
        // named-property box AT ALL, so no shadowing channel exists and the
        // guard's box clause only needs a word that can never read as
        // Object-tagged: the {byteOffset, length} word, whose top 16 bits
        // stay far below a pointer tag by construction (typed_array.h keeps
        // that word scan-safe for the collector, which is the same property).
        const Value memo = rtNativeMemberProbe(kind, keyIndex);
        if (memo.isUndefined() || memo.rawBits() != fnVal.rawBits()) return false;
        auxOffset = offsetof(TypedArrayHeader, byteOffset);
    } else if (kind == HeapKind::Function) {
        // A global constructor's static (`Array.isArray`, `String.raw`):
        // answered FIRST on the function-receiver ladder from a fixed C table
        // keyed by the RECEIVER'S code pointer, ahead even of the own-property
        // box (rt_prop.cpp) — nothing a program writes can shadow one, so the
        // answer is a pure function of (receiver code, key). The witness walks
        // the same table with raw code pointers (no allocation), and the hit
        // guard re-checks the live receiver's code word against the aux word —
        // the one identity a moving collector never rewrites.
        const auto* recvFn = reinterpret_cast<const FunctionHeader*>(objHdr);
        if (!recvFn->code) return false;
        if (!rtGlobalConstructorStaticMatches(recvFn->code, rtKeyString(keyIndex), fn->code)) {
            return false;
        }
        icEntry[BRONZE_ABI_METHOD_IC_AUX_WORD] = reinterpret_cast<uint64_t>(recvFn->code);
        auxOffset = offsetof(FunctionHeader, code);
        guardBits = BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT;
    } else {
        return false;
    }
    const uint64_t newWord0 = (auxOffset << BRONZE_ABI_METHOD_IC_BOX_SHIFT) |
                              (static_cast<uint64_t>(kind) << BRONZE_ABI_METHOD_IC_KIND_SHIFT) |
                              guardBits | BRONZE_ABI_METHOD_IC_EXOTIC_BIT;
    // An exotic install may still displace a PLAIN resident to way 1 — a
    // site mixing an array with a plain receiver keeps both entries.
    displaceMethodWay0(icEntry, newWord0);
    icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] = reinterpret_cast<uint64_t>(fn->code);
    icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD] = static_cast<uint64_t>(fn->arity);
    icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD] = BRONZE_ABI_UNDEFINED_BITS;
    icEntry[0] = newWord0;
    return true;
}

// The FUNCTION-receiver STATICS latch: the call twin of the property read's
// `installStaticsCacheEntry` (rt_prop_function.cpp), for the receivers the
// exotic latch above declines — a class or an ordinary function whose members
// live in a real, shape-indexed box hanging off `properties` rather than in a
// C table. `Object.defineProperty(this, …)` and `Object.defineProperties(this,
// …)` in three.js's `Object3D` constructor are the case that made it: %Object%
// is not one of `kCtors`, so nothing above could latch them and every instance
// built paid the helper twice.
//
// The entry it writes is the SLOT form, keyed on the BOX's shape, and the form
// is not a choice — it is the only one that stays sound here:
//
//   - DIRECT would cache a code pointer against a shape. Two different
//     classes' boxes can BE the same shape (`class A { static make(){} }` and
//     `class B { static make(){} }` share `{make}`), and so can a plain
//     object's, so a cached callee would answer for a receiver it was never
//     resolved from. SLOT caches only WHERE — and a shape is a key-to-slot map
//     and nothing else, so every object matching it holds the site's key at
//     that slot, in itself.
//   - SLOT also re-derives code, env and arity from the function object found
//     there at every hit, and re-checks that a Function is what is there at
//     all. That is what makes `A.make = somethingElse`, `delete A.make` (which
//     changes the box's shape) and a static replaced by a non-function all
//     land where they did before: the generated arm's own Function test, or
//     the helper's TypeError.
//
// Which is the same envelope the READ arm has, one check longer. So the two
// arms share a soundness argument as well as a predicate, and this adds no
// exposure the committed read side did not already carry.
//
// Way 1 never sees one of these: `displaceMethodWay0` refuses to promote a
// SLOT entry, so words 6-9 stay the plain-DIRECT residents the polymorphic arm
// expects, and generated code's statics arm reads way 0 only — the same
// way-0-only policy, for the same measured reason, as the read arm.
//
// Loads, compares and stores only, like everything else here.
bool latchFunctionStaticsMethodIc(uint64_t* icEntry, Value fnRecvVal, Value calleeVal,
                                  const InlineCache& probe) {
    // The probe is the scratch site the read just filled. Only
    // `installStaticsCacheEntry` fills it for a function receiver, and only for
    // an own data property of the box — so a real, non-flagged, depth-0 entry
    // here IS that fill, and anything else means the read was answered by the
    // ladder and there is nothing to cache.
    if (!probe.isRealShape() || probe.isAccessor() || probe.isAbsent()) return false;
    if (probe.realDepth() != 0) return false;
    const Value boxVal = fnRecvVal.asObject<FunctionHeader>()->properties;
    if (!boxVal.isObject()) return false;
    ObjectHeader* box = boxVal.asObject<ObjectHeader>();
    if (box->header.flags != HeapKind::Plain) return false;
    // The box the probe named must be the box the receiver has NOW: the read
    // ran between the fill and here, and a shape that has since transitioned
    // would make the cached slot an unrelated property's.
    if (box->shape != probe.cached_shape) return false;
    if (!rtStaticsBoxCacheable(fnRecvVal, box)) return false;
    // And the slot must still hold what the read returned. Cheap, and it is
    // the one line that ties the entry to the call about to be made rather
    // than to a lookup that agreed with it a moment ago.
    if (box->getSlot(probe.cached_slot).rawBits() != calleeVal.rawBits()) return false;

    const uint64_t shapeWord = reinterpret_cast<uint64_t>(box->shape);
    displaceMethodWay0(icEntry, shapeWord);
    icEntry[BRONZE_ABI_METHOD_IC_CODE_WORD] = 0;
    icEntry[BRONZE_ABI_METHOD_IC_ARITY_WORD] =
        (static_cast<uint64_t>(probe.cached_slot) + 1) << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
    icEntry[BRONZE_ABI_METHOD_IC_ENV_WORD] = BRONZE_ABI_UNDEFINED_BITS;
    icEntry[0] = shapeWord;
    return true;
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
// The installable forms, and why each is sound, are the ABI header's
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
//   non-Plain receiver -> EXOTIC form, latchExoticMethodIc above; and, for a
//               FUNCTION it declines, the SLOT form keyed on the receiver's
//               statics BOX rather than on the receiver, which has no shape
//               of its own (latchFunctionStaticsMethodIc above). The words
//               mean exactly what the contract says they mean — a shape, and
//               a slot in the object that shape describes; which object that
//               is, is decided by the arm the live receiver's flags select,
//               not by anything stored in the entry.
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
    if (!fnVal.isObject()) return;
    if (!thisVal.isObject()) {
        latchPrimitiveMethodIc(icEntry, thisVal, fnVal, probe);
        return;
    }
    auto* objHdr = thisVal.asObject<HeapObjectHeader>();
    auto* fnHdr = fnVal.asObject<HeapObjectHeader>();
    if (fnHdr->flags != HeapKind::Function) return;
    auto* fn = reinterpret_cast<FunctionHeader*>(fnHdr);
    if (!fn->code) return;
    if (objHdr->flags != HeapKind::Plain) {
        // A non-Plain receiver has no shape word of its OWN to guard on, so
        // nothing below this line may run for one. The exotic latch either
        // installs its kind-guarded form or leaves the entry untouched; a
        // FUNCTION it declines is then offered its statics BOX's shape, which
        // is a shape word after all — just not the receiver's. The two are
        // disjoint by construction (the exotic Function form serves exactly the
        // `kCtors` receivers `rtStaticsBoxCacheable` refuses), so the order is
        // for legibility rather than precedence.
        if (!latchExoticMethodIc(icEntry, objHdr, fnVal, fn, probe, keyIndex) &&
            objHdr->flags == HeapKind::Function) {
            latchFunctionStaticsMethodIc(icEntry, thisVal, fnVal, probe);
        }
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
            displaceMethodWay0(icEntry, shapeWord);
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
            displaceMethodWay0(icEntry, shapeWord);
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
        displaceMethodWay0(icEntry, shapeWord);
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
    CensusToken censusTok = nullptr;
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        // The method fetch IS this site's property read; the inner
        // bronze_prop_get below runs against a STACK scratch site and must
        // not mint a census row per call, so it is bracketed as nested.
        censusTok = censusRecordAccess(CensusKind::MethodGet, thisBits, keyIndex, 0, icEntry,
                                       BRONZE_CENSUS_RET_ADDR(), /*hasValue=*/false, 0);
        censusEnterNested();
    }
    uint64_t fnBits =
        bronze_prop_get(thisBits, keyIndex, reinterpret_cast<uint64_t*>(&scratch));
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        censusLeaveNested();
        censusRecordResult(censusTok, fnBits);
    }
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
    CensusToken censusTok = nullptr;
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        censusTok = censusRecordAccess(CensusKind::MethodGet, thisBits, keyIndex, 0, icEntry,
                                       BRONZE_CENSUS_RET_ADDR(), /*hasValue=*/false, 0);
        censusEnterNested();
    }
    uint64_t fnBits =
        bronze_prop_get(thisBits, keyIndex, reinterpret_cast<uint64_t*>(&scratch));
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        censusLeaveNested();
        censusRecordResult(censusTok, fnBits);
    }
    if (rtExceptionPending()) return BRONZE_ABI_UNDEFINED_BITS;

    recordCallSite("bronze_call_method_spread", fnBits);

    Rooted<Value> fnRoot{Value(fnBits)};
    latchMethodIc(icEntry, thisRoot.get(), fnRoot.get(), scratch.ways[0], keyIndex);

    return bronze_dynamic_call_spread(fnRoot.get().rawBits(), thisRoot.get().rawBits(),
                                      argsRoot.get().rawBits());
}

}  // extern "C"

}  // namespace bronze::runtime

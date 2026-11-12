// Computed element and property writes (`o[i] = v`): numeric array and
// typed-array fast paths, the computed-store inline cache (`elem_ic.h`),
// symbol-keyed writes, and receiver-kind dispatch for non-index keys.

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/accessor.h"
#include "runtime/array.h"
#include "runtime/elem_ic.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/namespace.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/shape_census.h"
#include "runtime/shape_leave.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

struct ElemSetFastEntry {
    const Shape* beforeShape{nullptr};
    const StringHeader* arenaKey{nullptr};
    uint32_t keyHash{0};
    InlineCache ic{};
};

constexpr uint32_t kElemSetFastBits = 12;
thread_local ElemSetFastEntry g_elemSetFastCache[1u << kElemSetFastBits];

inline ElemSetFastEntry& elemSetFastEntryFor(const Shape* shape, uint32_t keyHash) noexcept {
    uint64_t h = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shape)) >> 4) *
                     0x9E3779B97F4A7C15ULL ^
                 static_cast<uint64_t>(keyHash) * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return g_elemSetFastCache[h >> (64 - kElemSetFastBits)];
}

// Executes a cached computed store on a bare plain receiver (`sh` non-null and
// non-dictionary): own-slot
// overwrite, shape-transition add, or own/inherited accessor setter call.
bool tryCachedPlainElemSet(Value objVal, uint64_t idxBits, uint64_t valBits, bool strict,
                           InlineCache& ic, const StringHeader* matchedArenaKey) {
    auto* obj = objVal.asObject<ObjectHeader>();
    Shape* sh = obj->shape;
    if (ic.describesOwn(sh)) {
        obj->setSlot(ic.cached_slot, Value(valBits));
        return true;
    }
    if (!obj->isBarePlain() || rtMaybeGlobalObjectShape(sh)) return false;

    if (matchedArenaKey != nullptr && !sh->used_as_prototype && ic.isRealShape() &&
        !ic.isAccessor() && !ic.isAbsent() && ic.realDepth() == 0) {
        Shape* next = ic.cached_shape;
        const uint32_t slot = ic.cached_slot;
        const Value val(valBits);
        if (next->parent == sh && next->slot_index == slot && !next->repr_generalized &&
            (!ic.isDoubleSlot() || val.isNumber()) && next->enumerable && !next->accessor &&
            next->writable && next->configurable && ic.cached_epoch == protoMutationEpoch() &&
            (next->key.string() == matchedArenaKey ||
             (next->key.isString() && next->key.string()->equals(*matchedArenaKey)))) {
            noteShapeLeave(sh, next, ShapeLeave::Add);
            if (slot < ObjectHeader::kOverflowSlot) {
                obj->shape = next;
                obj->setSlot(slot, val);
                return true;
            }
            Rooted<Value> self{objVal};
            Rooted<Value> valRoot{val};
            ObjectHeader* live = ObjectHeader::ensureSlots(rtHeap(), self, slot + 1);
            live->shape = next;
            live->setSlot(slot, valRoot.get());
            return true;
        }
    }

    if (ic.isRealShape() && ic.describes(sh) && ic.isAccessor()) {
        const uint32_t depth = ic.realDepth();
        ObjectHeader* holder = obj;
        if (depth > 0) {
            bool crossedDictionary = false;
            holder = obj->cachedProtoHolder(depth, crossedDictionary);
        }
        if (holder) {
            Rooted<Value> holderRoot{Value::fromObject(holder)};
            Rooted<Value> recv{objVal};
            Rooted<Value> valRoot{Value(valBits)};
            Value setter = holderRoot.get().asObject<ObjectHeader>()->getSlot(ic.cached_slot + 1);
            Rooted<Value> fnRoot{setter};
            if (fnRoot.get().isObject() &&
                fnRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                FunctionHeader* fn = fnRoot.get().asObject<FunctionHeader>();
                if (fn->code && fn->arity == 1 && !fn->isNativeCode() &&
                    (fn->function_flags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) == 0) {
                    rtEnterJs(fn->code, fn->env_record.rawBits(), recv.get().rawBits(), 1,
                              reinterpret_cast<const uint64_t*>(valRoot.slot_ptr()));
                    return true;
                }
            }
            bool noSetter = false;
            callSetter(fnRoot.get(), recv, valRoot, &noSetter);
            if (noSetter) {
                const std::string keyText =
                    matchedArenaKey
                        ? rtUtf8Chars(matchedArenaKey)
                        : rtUtf8Chars(rtElemKeyAsString(Value(idxBits)).asString<StringHeader>());
                rtReportSetRefusal(SetRefusal::NoSetter, strict, keyText);
            }
            return true;
        }
    }
    return false;
}

}  // namespace

void rtForgetElemSetFastCache() noexcept {
    for (ElemSetFastEntry& e : g_elemSetFastCache) {
        e.beforeShape = nullptr;
        e.arenaKey = nullptr;
        e.ic = InlineCache{};
    }
}

extern "C" {

void bronze_elem_set(uint64_t objBits, uint64_t idxBits, uint64_t valBits, bool strict) {
    recordElemCall("bronze_elem_set", objBits, idxBits);
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled) && !Value(idxBits).isObject()) {
        // Object keys are skipped for elem_get's reason: ToPropertyKey runs
        // user code and the helper re-enters itself with the primitive.
        censusRecordAccess(CensusKind::ElemSet, objBits, /*keyIndex=*/0xFFFFFFFFu, idxBits,
                           /*siteId=*/nullptr, BRONZE_CENSUS_RET_ADDR(), /*hasValue=*/true,
                           valBits);
    }
    Value objVal(objBits);

    // 7.1.19 ToPropertyKey for an OBJECT key, at the entry and nowhere below,
    // for the reason `bronze_elem_get` does it here: the key's `toString` is
    // user code, and every branch past this point holds a raw receiver header
    // across the key conversion. The receiver AND the value are rooted, because
    // the value is just as movable as the receiver and is not touched again
    // until the write itself.
    if (Value(idxBits).isObject()) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> valRoot{Value(valBits)};
        Rooted<Value> keyRoot{Value(idxBits)};
        keyRoot.set(rtToPropertyKey(keyRoot));
        bronze_elem_set(objRoot.get().rawBits(), keyRoot.get().rawBits(),
                        valRoot.get().rawBits(), strict);
        return;
    }

    // Fast path: numeric index write on an Array or TypedArray without side-effects.
    if (objVal.isObject() && idxBits <= kNumberMaxBits) {
        const double d = std::bit_cast<double>(idxBits);
        const uint32_t idx = static_cast<uint32_t>(d);
        if (d >= 0.0 && static_cast<double>(idx) == d && d <= 4294967294.0) {
            HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
            if (hdr->flags == HeapKind::Array) {
                auto* arr = reinterpret_cast<ArrayHeader*>(hdr);
                // A hole is no own element, so a store into one asks the chain
                // (rtArrayIndexSet below) unless the hazard says no chain can
                // answer (object.h). So may an append at `length`: hazard clear,
                // extensible, in block = setElem's arm (branch_on_array_store).
                const bool clear = rtTls()->array_index_hazard == 0;
                if (idx < arr->length && idx < arr->capacity && !arr->properties.isObject() &&
                    (clear || !arr->elementsKeepingNumbers()[idx].isHole())) {
                    // In bounds, so NUMBERS survives a Number (runtime/array.h).
                    if (valBits > kNumberMaxBits) arr->forgetNumbers();
                    arr->elementsKeepingNumbers()[idx] = Value(valBits);
                    return;
                }
                if (clear && idx == arr->length && arr->head_offset + idx < arr->capacity &&
                    !arr->properties.isObject()) {
                    return arr->setElem(rtHeap(), idx, Value(valBits));
                }
            } else if (hdr->flags == TypedArrayHeader::kFlags && valBits <= kNumberMaxBits) {
                auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
                // A NUMBER into a BIGINT view is the one store that throws
                // (7.1.13), so this fast path — whose whole premise is that a
                // Number needs no conversion — is not for those two kinds.
                if (isBigIntElementKind(view->elementKind())) {
                    Rooted<Value> viewRoot{objVal};
                    rtTypedArraySetElement(viewRoot, idx, Value(valBits));
                    return;
                }
                const double num = std::bit_cast<double>(valBits);
                if (idx < view->length) {
                    view->set(idx, num);
                }
                return;  // out-of-bounds typed-array writes are discarded, per spec —
                         // including a view whose buffer was transferred or shrunk
                         // away, whose window closeOrReopenViews zeroed
            }
        }
    }

    // The computed-STORE cache (elem_ic.h). Asked here — after the array and
    // typed-array fast paths, before the receiver-kind ladder — because a
    // non-null entry proves the receiver PLAIN and the key a name, which is
    // every arm of that ladder answered at once. `elemSetCacheEntryFor`
    // guarantees the entry is about THIS (shape, key) pair, so `describesOwn`
    // has only the shape left to settle and a hit is the slot and nothing else:
    // no key stringification, no `Rooted`, no `Shape::lookupProperty` walk.
    //
    // Nothing this skips is owed. A refusal a plain receiver can raise —
    // frozen, sealed, non-writable, a String exotic's own index or `length` —
    // is either dictionary-mode (which the probe refuses outright) or a key
    // `setProp` was never allowed to reach, and `setProp` is the ONLY thing
    // that fills an entry. An accessor fills as an accessor, which
    // `describesOwn` refuses, so a setter still runs below.
    //
    // The entry outlives the allocations further down: it lives in a per-thread
    // table and holds only arena pointers, so no collection can move it or what
    // it names.
    // A map keyed by ids, which no entry below can speak for (a dictionary's
    // slots are not in its shape): the own writable data entry is the slot.
    if (const DictEntry* own = rtIdKeyedOwnEntry(objVal, Value(idxBits)); own && own->writable) {
        objVal.asObject<ObjectHeader>()->setSlot(own->slot, Value(valBits));
        return;
    }

    ElemSetCacheEntry* setEntry = elemSetCacheEntryFor(objVal, Value(idxBits));
    if (setEntry && setEntry->ic.describesOwn(objVal.asObject<ObjectHeader>()->shape)) {
        objVal.asObject<ObjectHeader>()->setSlot(setEntry->ic.cached_slot, Value(valBits));
        return;
    }

    if (setEntry &&
        tryCachedPlainElemSet(objVal, idxBits, valBits, strict, setEntry->ic, setEntry->key)) {
        return;
    }

    if (objVal.isObject() && Value(idxBits).isString()) {
        HeapObjectHeader* fastHdr = objVal.asObject<HeapObjectHeader>();
        if (fastHdr->flags == HeapKind::Plain) {
            auto* fastObj = reinterpret_cast<ObjectHeader*>(fastHdr);
            Shape* sh = fastObj->shape;
            if (fastObj->isBarePlain() && !sh->isDictionary() && !rtMaybeGlobalObjectShape(sh)) {
                StringHeader* strKey = Value(idxBits).asString<StringHeader>();
                const uint32_t strHash = strKey->hash();
                ElemSetFastEntry& fastEntry = elemSetFastEntryFor(sh, strHash);
                if (fastEntry.beforeShape == sh && fastEntry.keyHash == strHash &&
                    fastEntry.arenaKey != nullptr &&
                    (fastEntry.arenaKey == strKey || fastEntry.arenaKey->equals(*strKey))) {
                    if (tryCachedPlainElemSet(objVal, idxBits, valBits, strict, fastEntry.ic,
                                              fastEntry.arenaKey)) {
                        return;
                    }
                }
                if (!sh->used_as_prototype) {
                    PropertyInfo own;
                    if (sh->lookupProperty(PropertyKey::forString(strKey), own) &&
                        !own.accessor && own.writable) {
                        fastObj->setSlot(own.slot, Value(valBits));
                        Shape* postSh = fastObj->shape;
                        if (!censusFillsSuppressed() && postSh && !postSh->isDictionary() &&
                            !postSh->used_as_prototype) {
                            InlineCache filled{};
                            filled.fillForSet(postSh, own.slot, postSh->slotIsDouble(own.slot));
                            if (setEntry) setEntry->ic = filled;
                            const StringHeader* ak =
                                (setEntry && setEntry->key)
                                    ? setEntry->key
                                    : (strKey->isArena() ? strKey : nullptr);
                            if (ak) {
                                ElemSetFastEntry& dst = elemSetFastEntryFor(postSh, strHash);
                                dst = ElemSetFastEntry{postSh, ak, strHash, filled};
                            }
                        }
                        return;
                    }
                }
            }
        }
    }

    if (Value(idxBits).isSymbol()) {
        if (objVal.isNull() || objVal.isUndefined()) {
            rtThrowTypeError("Cannot set properties of " +
                             std::string(objVal.isNull() ? "null" : "undefined") +
                             " (setting a symbol-keyed property)");
            return;
        }
        // A proxy's set trap takes a symbol like any key; the holder search
        // below knows only objects with a shape.
        if (objVal.isObject() && objVal.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            rtProxySet(objVal, Value(idxBits), Value(valBits), strict);
            return;
        }
        Rooted<Value> recv{objVal};
        // Rooted BEFORE the side-object build below, which allocates. The key
        // needs no root — a symbol lives in the arena and never moves — but the
        // VALUE is an ordinary heap value, and rooting it after the allocation
        // would root a pointer the collector had already moved. That is exactly
        // what it did: `f[sym] = "bare"` on a fresh function read back as an
        // unrelated string under BRONZE_GC_STRESS=1.
        Rooted<Value> val{Value(valBits)};
        // A function's side object of statics is built on first demand, so a
        // symbol-keyed write can be the demand — without this, `f[sym] = v` on
        // a function that had never been given a static reached the "no shape"
        // error below and the write was refused for a receiver that can hold
        // one perfectly well.
        if (recv.get().isObject() &&
            recv.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            rtPrepareFunctionOwnWrite(recv);
        }
        if (recv.get().isObject() &&
            recv.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
            ArrayHeader::ensureProperties(rtHeap(), rtArena(), recv);
        }
        if (ObjectHeader* holder = rtSymbolKeyHolder(recv.get())) {
            Rooted<Value> holderRoot{Value::fromObject(holder)};
            Rooted<Value> key{Value(idxBits)};
            SetRefusal refusal = SetRefusal::None;
            holderRoot.get().asObject<ObjectHeader>()->setProp(
                rtHeap(), rtArena(), key, val, /*ic=*/nullptr, /*enumerable=*/true,
                /*defineOwn=*/false, recv.slot_ptr(), &refusal);
            // A symbol has no spelling a message can quote back — its
            // description is not its identity — so the position is named
            // instead of the key.
            rtReportSetRefusal(refusal, strict, "<symbol>");
            return;
        }
        // A namespace refuses a SYMBOL key on the same terms as a string one:
        // 10.4.6.9 returns false without ever looking at the key.
        if (rtModuleNamespaceWriteRefused(recv.get(), "<symbol>", strict)) return;
        rtThrowTypeError("a symbol-keyed property write is only supported on a plain object, a "
                         "function or an array (a typed array carries no shape at all)");
        return;
    }
    // A write through `o[i]` to something that is not an object, answered
    // exactly as `bronze_prop_set` answers `o.k`: the nullish TypeError first,
    // then `rtPrimitiveWrite`. They are one operation with two spellings, and the
    // read side has already been made to agree (`cases/string_index`); a
    // `fatal` here would kill a process where the `o.k` spelling of the same
    // write is a value a `catch` can hold.
    if (!objVal.isObject()) {
        Rooted<Value> recv{objVal};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
        if (recv.get().isNull() || recv.get().isUndefined()) {
            rtThrowTypeError("Cannot set properties of " +
                             std::string(recv.get().isNull() ? "null" : "undefined") +
                             " (setting '" + keyText + "')");
            return;
        }
        Rooted<Value> val{Value(valBits)};
        rtPrimitiveWrite(recv, key, keyText, val, strict);
        return;
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;
    if (hdr->flags == HeapKind::Proxy) {
        // One operation, two spellings, like every arm below: `p[k] = v` is
        // `p.k = v` and takes the same trap-or-forward path.
        rtProxySet(objVal, Value(idxBits), Value(valBits), strict);
        return;
    }
    Value idxVal(idxBits);
    if (idxVal.isNumber()) {
        double d = idxVal.asNumber();
        if (d >= 0.0 && d <= 4294967294.0) {
            uint32_t u = static_cast<uint32_t>(d);
            if (static_cast<double>(u) == d) {
                if (hdr->flags == HeapKind::Array) {
                    rtArrayIndexSet(objVal, u, Value(valBits), strict);
                    return;
                }
                if (hdr->flags == TypedArrayHeader::kFlags) {
                    // The conversion (ToNumber, or ToBigInt for the two 64-bit
                    // integer views) can be a user `valueOf`; the funnel
                    // re-derives the view from the root after it.
                    Rooted<Value> viewRoot{objVal};
                    rtTypedArraySetElement(viewRoot, u, Value(valBits));
                    return;
                }
            }
        }
    }
    if (hdr->flags == HeapKind::Array) {
        if (!rtValueToElementIndex(Value(idxBits), idx)) {
            // A key that is not a canonical array index NAMES a property, and
            // `a[k] = v` means exactly what `a.k = v` means — so it takes the
            // same two arms, through the same file, rather than keeping a
            // second opinion about what `a["length"]` is.
            Rooted<Value> arrRoot{objVal};
            Rooted<Value> val{Value(valBits)};
            Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
            const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
            if (keyText == "length") {
                rtReportSetRefusal(rtArraySetLength(arrRoot, val.get()), strict, keyText);
                return;
            }
            rtReportSetRefusal(rtArrayNamedSet(arrRoot, key, val), strict, keyText);
            return;
        }
        rtArrayIndexSet(objVal, idx, Value(valBits), strict);
        return;
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        // A NUMBER key that reaches here failed the exact-uint32 fast path
        // above, so it is NaN, negative, fractional or >= 2^32-1. On an
        // integer-indexed exotic object every number is a NUMERIC index
        // (10.4.5.5 through CanonicalNumericIndexString), and an invalid one
        // is the spec's silently-discarded store — never a named property.
        // Only an actual string key can name one.
        // TypedArraySetElement (10.4.5.16) converts the VALUE before it
        // looks at the index, so the discarded store still runs ToNumber.
        if (Value(idxBits).isNumber()) {
            Rooted<Value> viewRoot{objVal};
            rtTypedArraySetElement(viewRoot, kNoTypedArrayIndex, Value(valBits));
            return;
        }
        if (rtValueToElementIndex(Value(idxBits), idx)) {
            Rooted<Value> viewRoot{objVal};
            rtTypedArraySetElement(viewRoot, idx, Value(valBits));
            return;  // out-of-bounds typed-array writes are discarded, per spec
        }
        // A string that spells a canonical number without being a valid index
        // is the same discarded store (10.4.5.5 step 1.b); every other string
        // and every symbol is an ordinary property of the view, written by the
        // shape-carrying tail below.
        if (Value(idxBits).isString() &&
            rtIsCanonicalNumericString(rtUtf8Chars(Value(idxBits).asString<StringHeader>()))) {
            Rooted<Value> viewRoot{objVal};
            rtTypedArraySetElement(viewRoot, kNoTypedArrayIndex, Value(valBits));
            return;
        }
    }
    if (hdr->flags == HeapKind::Function) {
        Rooted<Value> fnRoot{objVal};
        Rooted<Value> val{Value(valBits)};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        const std::string keyText = rtUtf8Chars(key.get().asString<StringHeader>());
        if (keyText == "prototype") {
            if (!rtFunctionPrototypeWritable(fnRoot.get())) {
                // A REFUSAL, not a diagnosis of the value: 10.1.9.2 discards
                // the write in sloppy code and 13.15.2 throws in strict, and
                // that is true whatever was being assigned. The `fatal` that
                // used to stand here said "assigning a non-object", which was
                // the wrong sentence about the right situation.
                rtReportSetRefusal(SetRefusal::NotWritable, strict, keyText);
                return;
            }
            auto* fn = fnRoot.get().asObject<FunctionHeader>();
            fn->prototype = val.get();
            fn->instance_shape =
                val.get().isObject() ? rtNewRootShape(val.get(), rtInstanceInlineSlots(fn)) : nullptr;
            return;
        }
        // 10.2.9 SetFunctionName and 10.2.10 SetFunctionLength make `name` and
        // `length` NON-WRITABLE own properties of every function, so the write
        // is discarded in sloppy code and a TypeError in strict — never a
        // store. `bronze_prop_set` has always said so for `f.name = v`; without
        // it here the same write spelled `f[k] = v` landed in the statics
        // table, which the read consults FIRST, and the two spellings of one
        // assignment disagreed about whether it happened.
        if (fnRoot.get().asObject<FunctionHeader>()->name != nullptr &&
            (keyText == "length" || keyText == "name")) {
            rtReportSetRefusal(SetRefusal::NotWritable, strict, keyText);
            return;
        }
        rtPrepareFunctionOwnWrite(fnRoot);
        Rooted<Value> propsRoot{fnRoot.get().asObject<FunctionHeader>()->properties};
        SetRefusal refusal = SetRefusal::None;
        propsRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                         /*ic=*/nullptr, /*enumerable=*/true,
                                                         /*defineOwn=*/false,
                                                         /*receiver=*/nullptr, &refusal);
        rtReportSetRefusal(refusal, strict, keyText);
        return;
    }
    if (HeapKind::carriesShape(hdr->flags)) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> val{Value(valBits)};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        // `s[0] = "z"`, the spelling this bug actually arrives in. Same answer
        // as `bronze_prop_set`'s, for the reason the two TypeErrors above are
        // the same: one operation, two spellings. Asked of the WRAPPER first,
        // which is one flag test, so the key's `std::string` is built only for
        // the receiver that can actually refuse — this is the miss path of
        // every computed store, and it used to copy the key on all of them.
        if (Value stringData; rtStringWrapperData(objRoot.get(), stringData)) {
            if (rtStringDataWriteRefused(stringData,
                                         rtUtf8Chars(key.get().asString<StringHeader>()), strict)) {
                return;
            }
        }
        Shape* preShape = objRoot.get().asObject<ObjectHeader>()->shape;
        const bool trackFastStringIc =
            Value(idxBits).isString() && hdr->flags == HeapKind::Plain &&
            objRoot.get().asObject<ObjectHeader>()->isBarePlain() && preShape &&
            !preShape->isDictionary() && !rtMaybeGlobalObjectShape(preShape);
        InlineCache localIc{};
        InlineCache* icSlot = setEntry ? &setEntry->ic : (trackFastStringIc ? &localIc : nullptr);
        SetRefusal refusal = SetRefusal::None;
        objRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val,
                                                        icSlot,
                                                        /*enumerable=*/true,
                                                        /*defineOwn=*/false,
                                                        /*receiver=*/nullptr, &refusal);
        if (trackFastStringIc && refusal == SetRefusal::None && icSlot && icSlot->isRealShape()) {
            StringHeader* liveKey = key.get().asString<StringHeader>();
            const StringHeader* ak =
                (setEntry && setEntry->key)
                    ? setEntry->key
                    : (liveKey->isArena() ? liveKey : elemCacheInternKey(liveKey));
            if (ak) {
                const uint32_t strHash = ak->hash();
                ElemSetFastEntry& dst = elemSetFastEntryFor(preShape, strHash);
                dst = ElemSetFastEntry{preShape, ak, strHash, *icSlot};
            }
        }
        // Likewise: the key text is what the message quotes, and there is no
        // message unless the write was refused.
        if (refusal != SetRefusal::None) {
            rtReportSetRefusal(refusal, strict, rtUtf8Chars(key.get().asString<StringHeader>()));
        }
        return;
    }
    {
        // `ns[k] = v` is the same operation as `ns.k = v` and gets the same
        // answer, for the reason the two TypeErrors above are shared: one
        // operation, two spellings. The key is only needed for the message.
        Rooted<Value> nsRoot{objVal};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        if (rtModuleNamespaceWriteRefused(nsRoot.get(),
                                          rtUtf8Chars(key.get().asString<StringHeader>()),
                                          strict)) {
            return;
        }
    }
    rtThrowTypeError("computed index writes are only supported on arrays, plain objects "
                     "and typed arrays");
}

}  // extern "C"

}  // namespace bronze::runtime

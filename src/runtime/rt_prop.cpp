// Property and element READS: the `o.k` and `o[i]` halves of the ABI, and the
// key decoding both directions share.
//
// Each receiver kind is its own branch because each stores properties
// differently — an array in its elements, a typed array in its buffer, a
// function in its prototype slot and own-property object, a plain object in
// its shape and slots. A name the receiver's prototype really defines and
// bronze has not built is diagnosed by rt_members.cpp rather than read as
// `undefined`.
//
// A PRIMITIVE receiver is the one kind that is not here, and the seam is that
// same sentence read backwards: it stores nothing, so its answer comes from an
// intrinsic prototype rather than from the value. rt_prop_primitive.cpp owns it.
//
// A SYMBOL key is the other thing that is not here, and its seam is the key
// rather than the receiver: every kind keeps symbol-keyed properties the same
// way, so what stays interesting is the handful of well-known symbols whose
// intrinsic prototype bronze does not build. rt_prop_symbol.cpp stands in for
// those objects; this file calls it once, from the symbol arm below.
//
// The WRITE dispatch is rt_prop_write.cpp, split off along the same kind of
// seam: a read asks every receiver the same question and takes each kind's
// answer, while a write asks whether the receiver can hold the property at all
// — so that file is almost entirely refusals and this one is almost entirely
// lookups. What they SHARE is the key — whether it names an element and what
// string a computed one names mean the same thing in either direction — and
// that is rt_key.cpp, reached by both through rt_property.h so that neither can
// keep a second opinion about `a["01"]`.

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include "abi/bronze_abi.h"
#include "runtime/tls_block.h"
#include "runtime/accessor.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/profile.h"
#include "runtime/shape_census.h"
#include "runtime/ic_log.h"
#include "runtime/exception.h"
#include "runtime/proxy.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/iterator.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/namespace.h"
#include "runtime/native_base.h"
#include "runtime/promise.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/elem_ic.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The IC table is a zero-initialized global array in the GENERATED object file
// (`__bronze_ic_table`, one BRONZE_ABI_IC_SITE_SIZE site per property or
// method site), and `rtAsCache` (rt_property.h) takes the site's way-0 entry
// pointer. That is what lets compiled code hold a stable address per site and
// inline the shape check, which a std::vector — which reallocates — could never
// offer.
//
// `entry` is null only when a caller has no site to cache against (the
// runtime's own property paths); ObjectHeader::getProp already treats a null
// cache as "look it up and cache nothing", a difference in speed and not in
// semantics. It is a real site pointer or null and nothing else — the backend
// never passes an index or a placeholder in its place.

extern "C" {

// A property read by NAME, with the receiver-kind dispatch that `o.k` and
// `o[k]` must share. They reach it from two different places - one with the
// key the compiler registered, one with the key ToPropertyKey just produced -
// and a second copy of this dispatch would be a second answer to "does this
// member exist?", which is the question rt_members.cpp exists to keep one of.
static uint64_t propGetByName(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                              InlineCacheSite* site);

static uint64_t propGetHelperBody(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry) {
    recordPropCall("bronze_prop_get", keyIndex, icEntry);
    recordPropGetMiss(objBits, keyIndex, icEntry);
    Value objVal(objBits);
    InlineCacheSite* site = rtAsCacheSite(icEntry);
    InlineCache* ic = rtAsCache(icEntry);

    // IC-hit fast path first: a shape match needs no key at all. Generated code
    // inlines the depth-0/inline-slot corner of exactly this check and only
    // calls in when that misses, so what remains hot here is the proto-hit and
    // overflow-slot case.
    if (objVal.isObject()) {
        HeapObjectHeader* fastHdr = objVal.asObject<HeapObjectHeader>();
        if (fastHdr->flags == TypedArrayHeader::kFlags) {
            // The two questions a typed array answers from its KIND before the
            // ordinary walk below (10.4.5): an integer index is an element,
            // and `length` — with `byteLength`, `byteOffset` and `buffer` —
            // is read off the header when the chain is pristine, which is
            // every view a program has not subclassed, decorated or
            // redefined an accessor of (rt_receivers.h). Anything else falls
            // through to the shaped path: a view carries a shape, so a method
            // read is a site-cached prototype hit like any object's.
            const KeyInfo& ki = rtKeyInfo(keyIndex);
            if (ki.isElemIndex) {
                // Through rtTypedArrayElement rather than `view->get`, because
                // a BigInt64/BigUint64 element is a BigInt and has to be
                // allocated: one funnel, so no read path can come to believe
                // every element is a double.
                return rtTypedArrayElement(Value(objBits), ki.elemIndex).rawBits();
            }
            const auto* view = reinterpret_cast<const TypedArrayHeader*>(fastHdr);
            if (ki.isLength) {
                if (rtTypedArrayChainPristine(view)) {
                    return Value::fromDouble(view->length).rawBits();
                }
            } else {
                StringHeader* keyHeader = rtKeyHeader(keyIndex);
                if (keyHeader && keyHeader->isLatin1()) {
                    const size_t kLen = keyHeader->getLength();
                    const char* kData = keyHeader->latin1Data();
                    if (kLen == 10 && std::memcmp(kData, "byteLength", 10) == 0 &&
                        rtTypedArrayChainPristine(view)) {
                        return Value::fromDouble(view->byteLength()).rawBits();
                    }
                    if (kLen == 10 && std::memcmp(kData, "byteOffset", 10) == 0 &&
                        rtTypedArrayChainPristine(view)) {
                        // 23.2.3.3: +0 for a view its buffer left behind —
                        // the one length-family answer the maintained window
                        // cannot carry, because the stored offset survives
                        // the closing.
                        return Value::fromDouble(view->isOutOfBounds() ? 0.0 : view->byteOffset)
                            .rawBits();
                    }
                    if (kLen == 6 && std::memcmp(kData, "buffer", 6) == 0 &&
                        rtTypedArrayChainPristine(view)) {
                        return view->buffer.rawBits();
                    }
                }
            }
        }
        if (HeapKind::carriesShape(fastHdr->flags)) {
            auto* fastObj = reinterpret_cast<ObjectHeader*>(fastHdr);
            // `site` is the caller's own entry in its module's IC table, or
            // null for the runtime's internal reads, which cache nothing.
            // The site's OTHER ways, which generated code also scanned and
            // also missed — so reaching here means every way disagreed with
            // the receiver's shape, or the one that matched needs a walk the
            // inline path refuses. Scanned again rather than passed in,
            // because the helper is entered from paths that never ran the
            // inline scan at all.
            if (site) ic = site->find(fastObj->shape, rtIcWayLimit());
            if (ic && ic->describesAbsent(fastObj->shape)) {
                return BRONZE_ABI_UNDEFINED_BITS;
            }
            if (ic && ic->cached_shape) {
                if (ic->describes(fastObj->shape)) {
                    if (ic->isAccessor()) {
                        uint32_t depth = ic->realDepth();
                        ObjectHeader* holder = fastObj;
                        if (depth > 0) {
                            bool crossedDictionary = false;
                            holder = fastObj->cachedProtoHolder(depth, crossedDictionary);
                        }
                        if (holder) {
                            Rooted<Value> holderRoot{Value::fromObject(holder)};
                            Rooted<Value> self{objVal};
                            Value getter = holderRoot.get().asObject<ObjectHeader>()->getSlot(ic->cached_slot);
                            Rooted<Value> fnRoot{getter};
                            if (fnRoot.get().isObject() &&
                                fnRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                                FunctionHeader* fn = fnRoot.get().asObject<FunctionHeader>();
                                if (fn->code && fn->arity == 0) {
                                    return rtEnterJs(fn->code, fn->env_record.rawBits(), self.get().rawBits(), 0, nullptr);
                                }
                            }
                            return callGetter(fnRoot.get(), self).rawBits();
                        }
                    } else {
                        if (ic->cached_depth == 0) return fastObj->getSlot(ic->cached_slot).rawBits();
                        bool crossedDictionary = false;
                        if (ObjectHeader* holder =
                                fastObj->cachedProtoHolder(ic->cached_depth, crossedDictionary)) {
                            return holder->getSlot(ic->cached_slot).rawBits();
                        }
                    }
                }
            }
        } else if (fastHdr->flags == HeapKind::Array) {
            if (ic && ic->isArrayMethod() && rtTls()->array_method_ic_enabled != 0) {
                const auto* arr = reinterpret_cast<const ArrayHeader*>(fastHdr);
                if (!arr->properties.isObject()) {
                    return rtTls()->array_method_tbl[ic->cached_slot];
                }
            }
            const KeyInfo& ki = rtKeyInfo(keyIndex);
            if (ki.isElemIndex) {
                return reinterpret_cast<const ArrayHeader*>(fastHdr)->getElem(ki.elemIndex).rawBits();
            }
            if (ki.isLength) {
                return Value::fromDouble(reinterpret_cast<const ArrayHeader*>(fastHdr)->length).rawBits();
            }
        }
    } else if (objVal.isString()) {
        // A STRING receiver, which generated code's inline IC can never hit
        // (it guards on an object's shape word, and a string has none), so
        // every `s.length` and every `s.charCodeAt` of a string-walking loop
        // arrives here. Before this branch each one took the by-name path:
        // `String.prototype` walked by `Shape::lookupProperty`, a content
        // compare per key on the way — ~400 ns a read, 120x node on a seeded
        // hash. 10.4.3.4's own `length` is answered from the header, and a
        // method is answered from the site's cache of `String.prototype`'s
        // shape, which `stringMember`'s walk filled on the first miss: the
        // receiver is not the holder, so the entry is read against the
        // intrinsic's shape instead of the receiver's, at depth 0 only (the
        // intrinsic's own members; anything deeper takes the walk).
        const KeyInfo& ki = rtKeyInfo(keyIndex);
        if (ki.isLength) {
            return Value::fromDouble(objVal.asString<StringHeader>()->getLength()).rawBits();
        }
        if (!ki.isElemIndex && site && site->ways[0].isRealShape()) {
            // The filled way is usually the intrinsic's own, but a site that
            // saw a plain object first may meet its first string before any
            // string walk built `String.prototype` — and building it
            // allocates, so the receiver (a movable heap string) is rooted
            // across the fetch and re-read after it.
            Rooted<Value> self{objVal};
            const Value proto = rtStringPrototype();
            objVal = self.get();
            if (proto.isObject()) {
                auto* holder = proto.asObject<ObjectHeader>();
                InlineCache* hit = site->find(holder->shape, rtIcWayLimit());
                if (hit && hit->describesOwn(holder->shape)) {
                    return holder->getSlot(hit->cached_slot).rawBits();
                }
            }
        }
    }

    return propGetByName(objVal, rtKeyString(keyIndex), rtKeyHeader(keyIndex), site);
}

uint64_t bronze_prop_get(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry) {
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        // Receiver identity is read BEFORE the body: the walk below can
        // allocate, and a collection would move the receiver out from under a
        // post-hoc read. The RESULT is classified after, from its bits alone.
        CensusToken tok =
            censusRecordAccess(CensusKind::PropGet, objBits, keyIndex, 0, icEntry,
                               BRONZE_CENSUS_RET_ADDR(), /*hasValue=*/false, 0);
        const uint64_t r = propGetHelperBody(objBits, keyIndex, icEntry);
        censusRecordResult(tok, r);
        return r;
    }
    return propGetHelperBody(objBits, keyIndex, icEntry);
}

static uint64_t propGetByName(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                              InlineCacheSite* ic) {
    // The interned key is needed by more than the plain-object branch now, so
    // its registration is checked once at the top rather than where it is
    // first read.
    if (!keyHeader) fatal("property access with an unregistered key index");

    // Reading a property of null or undefined is a TypeError in ECMA-262 7.3.2
    // (GetV -> ToObject), and answering `undefined` for it is the
    // silent-wrong-answer shape CLAUDE.md forbids: `a.b.c` where `a.b` is
    // missing would report nothing and carry an undefined onward. It is also
    // what makes `(a?.b).c` differ observably from `a?.b.c`. Catchable since
    // The spec names it, so it is a thrown TypeError rather than the process
    // death it used to be.
    if (objVal.isNull() || objVal.isUndefined()) {
        return rtThrowTypeError("Cannot read properties of " +
                                std::string(objVal.isNull() ? "null" : "undefined") +
                                " (reading '" + keyStr + "')")
            .rawBits();
    }
    // Every other PRIMITIVE receiver, whose answer comes from an intrinsic
    // rather than from the value — rt_prop_primitive.cpp owns that whole
    // question, including the index properties 10.4.3.5 synthesises for a
    // string.
    if (!objVal.isObject()) {
        return rtPrimitiveMember(objVal, keyStr, keyHeader, ic).rawBits();
    }

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    uint32_t idx = 0;

    if (hdr->flags == HeapKind::Array) {
        // Rooted for the tail: everything from the property-object walk onward
        // can allocate, and the `Object.prototype` step below needs the
        // receiver AFTER those allocations have possibly moved it. `arr` is
        // read only above the first of them.
        Rooted<Value> recv{objVal};
        ArrayHeader* arr = recv.get().asObject<ArrayHeader>();
        if (keyStr == "length") return Value::fromDouble(arr->length).rawBits();
        if (rtKeyAsIndex(keyStr, idx)) return arr->getElem(idx).rawBits();
        // A named own property: a match array's `index`, an `arguments`
        // object's `callee`, or anything a program assigned. Read BEFORE the
        // prototype methods, because an own property SHADOWS an inherited one —
        // `a.map = 5` reads 5, and `m.index` must not answer with
        // `Array.prototype.index` if one is ever added.
        //
        // The presence test is the shape's and not "the value is not
        // undefined": `a.map = undefined` is an own property whose value is
        // undefined, and reading the builtin for it would un-shadow a property
        // the program really created.
        //
        // The same read continues UP the box's chain, which for a subclass
        // instance is `MySubArray.prototype` and everything above it — that is
        // where an Array subclass's [[Prototype]] lives, since an array carries
        // no shape of its own (runtime/native_base.h). An array with no box —
        // every array a program has neither subclassed nor written a named
        // property on — answers after one load, which is what keeps the method
        // table below the only thing an ordinary `a.map` touches.
        if (Value own; rtExoticNamedRead(recv, keyHeader, own)) return own.rawBits();
        if (uint32_t methodId = rtArrayMethodId(keyStr); methodId != UINT32_MAX) {
            Value method = rtArrayMethodById(methodId);
            if (!method.isUndefined()) {
                // Way 0, always: generated code's array arm looks there and
                // nowhere else, and InlineCacheSite::slotForInstall keeps a
                // sentinel it finds at way 0 from being shifted away.
                if (ic && rtTls()->array_method_ic_enabled != 0 &&
                    !recv.get().asObject<ArrayHeader>()->properties.isObject()) {
                    ic->ways[0].fillArrayMethod(methodId);
                }
                return method.rawBits();
            }
        }
        rtCheckArrayMember(keyStr);
        // `Array.prototype`'s own members have all had their say — including
        // the two that SHADOW this next step, `toString` and `toLocaleString`,
        // which the table above refuses by name. So what is left is the chain
        // above it.
        return rtObjectProtoMember(recv, keyStr).rawBits();
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        // 10.4.5.4 [[Get]]: a NUMERIC key is an element or an absence — never
        // a name the chain answers. The integer index is tried FIRST, because
        // `v[0]` is the whole point of a typed array; out of range is
        // `undefined` and not an error. A canonical numeric string that is
        // not a valid index ("1.5", "-0", "NaN") is the same absence. Every
        // other name is the ordinary walk below: the view carries a shape.
        if (rtKeyAsIndex(keyStr, idx)) return rtTypedArrayElement(objVal, idx).rawBits();
        if (rtIsCanonicalNumericString(keyStr)) return BRONZE_ABI_UNDEFINED_BITS;
    }
    if (hdr->flags == RegExpHeader::kFlags) {
        // 22.2.4.1: `lastIndex` is the one own data property a RegExp always
        // has, and it lives in the header rather than in a slot so the exec
        // path reads it with no lookup. Every other name is the ordinary walk
        // below — the RegExp carries a shape whose chain is `RegExp.prototype`.
        if (keyStr == "lastIndex") return rtRegExpLastIndexValue(objVal).rawBits();
    }
    if (hdr->flags == IterRecordHeader::kFlags) {
        // The record of a live for-of is not a JS value: nothing hands one to
        // a program, so reaching this is a lowering bug rather than something
        // a program did.
        fatal("internal: a property read on an iteration record");
    }
    if (hdr->flags == ModuleNamespaceHeader::kFlags) {
        // 10.4.6.7. A namespace has no prototype (10.4.6.1 fixes [[Prototype]]
        // at null), so there is no chain to continue on and a name it does not
        // export is `undefined` here rather than a step further up.
        Value found = Value::fromUndefined();
        // Cannot answer false — the flags word above is what it tests — so this
        // returns unconditionally rather than falling through to a cast that
        // would read a namespace's payload as an ObjectHeader's shape word.
        rtModuleNamespaceGet(objVal, keyHeader, found);
        return found.rawBits();
    }
    if (hdr->flags == HeapKind::Proxy) {
        // 10.5.8: the `get` trap, or the target's read through this same
        // funnel. The proxy has no shape of its own, so nothing below this
        // dispatch could answer for it anyway.
        return rtProxyGet(objVal, Value::fromString(keyHeader)).rawBits();
    }
    if (hdr->flags == HeapKind::Function) {
        // Four places in a fixed order, and the order is the whole answer — so
        // it lives in one file of its own (rt_prop_function.cpp) rather than as
        // the longest branch of this dispatch. It is handed the SITE because
        // one of those four places, the function's statics object, is a shape
        // and a slot and can therefore be cached.
        return rtFunctionMember(objVal, keyStr, keyHeader, ic);
    }

    // Interned arena key: no allocation on the property path.
    //
    // The RECEIVER is rooted because a read can now run user code: a getter is
    // a call, so this load is a collection point like any other helper call,
    // and the raw bits this helper was handed are dead the moment one runs.
    // Every kind with a branch above returned; anything else reaching the
    // plain-object tail would be read through an ObjectHeader it is not, so the
    // cast is guarded rather than trusted. `bronze_elem_get` used to hold this
    // guarantee for the computed path and lost it when that path was folded
    // into this one. The byte-store family (a view, a buffer, a DataView) opens
    // with an ObjectHeader and is read through it like any ordinary object.
    if (!HeapKind::carriesShape(hdr->flags)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "internal: a property read on an unknown object kind: flags=%u, key='%.*s'",
                      (unsigned)hdr->flags, (int)keyStr.size(), keyStr.data());
        fatal(buf);
    }
    Rooted<Value> objRoot{objVal};
    // 10.4.3.5 StringGetOwnProperty, ahead of the ordinary lookup for a String
    // exotic object. The order is 10.4.3's and not an optimisation: the index
    // properties and the `length` 10.4.3.4 synthesises are non-writable and
    // non-configurable, so no own property a program can define shadows them.
    // A key this does not claim falls through to the ordinary walk, which is
    // the half `new String("ab").indexOf` needs and the half bronze had no
    // holder for before `String.prototype` became an object.
    if (Value exotic; rtStringExoticOwnProperty(objRoot.get(), keyStr, exotic)) {
        return exotic.rawBits();
    }
    Rooted<Value> key(Value::fromString(keyHeader));
    // The walk reports whether it found the key NOWHERE and ran to the chain's
    // end. Asked of the walk rather than re-derived afterwards: a second walk
    // would repeat a shape lookup per link, which measured 60% on a site that
    // rotates through more shapes than the cache holds ways.
    bool absentThroughChain = false;
    Value result = objRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key, ic,
                                                                  /*receiver=*/nullptr,
                                                                  &absentThroughChain);
    // A namespace object is an ordinary object, so a member it does not carry
    // reads `undefined` like any other miss — which for a name ECMA-262 says
    // exists is the silent lie rt_members.cpp exists to prevent. Checked only
    // on the miss, so the hit path is untouched.
    if (result.isUndefined()) {
        // %GeneratorFunction.prototype% and its two siblings inherit from a
        // FUNCTION object, a link the walk above stops at. Their inherited
        // members are answered here, uncached: the answer is a real value the
        // chain holds, not a miss.
        if (Value inherited; rtFunctionKindInheritedMember(objRoot, keyHeader, keyStr, inherited)) {
            return inherited.rawBits();
        }
        // Each of these answers whether it CLAIMED this receiver as the
        // singleton whose absent members it diagnoses from a table. They are
        // OR'd rather than short-circuited so that every one still runs: the
        // claim decides only whether the miss may be cached, never whether the
        // diagnostic fires.
        bool diagnosed = rtMathCheckMissingMember(objRoot.get(), keyStr);
        diagnosed = rtPerformanceCheckMissingMember(objRoot.get(), keyStr) || diagnosed;
        diagnosed = rtAtomicsCheckMissingMember(objRoot.get(), keyStr) || diagnosed;
        diagnosed = rtObjectCheckMissingMember(objRoot.get(), keyStr) || diagnosed;
        diagnosed = rtJsonCheckMissingMember(objRoot.get(), keyStr) || diagnosed;
        // The `Array.prototype` object, whose misses are Array's table: a name
        // 23.1.3 defines and bronze has not built must be as loud read off the
        // object as it is read off an array.
        diagnosed = rtArrayPrototypeCheckMissingMember(objRoot.get(), keyStr) || diagnosed;
        // And the chain's own end: a 20.1.3 member of `Object.prototype` that
        // bronze has not built. Applied to every plain object because every
        // plain object inherits from it — an own or nearer property of the same
        // name was found above and never reaches here. Receiver-independent, so
        // it makes no claim: a key it lets through here it lets through for
        // every plain object, cached or not.
        rtObjectProtoCheckMissingMember(keyStr);
        // The walk found nothing anywhere. Say so in the site's cache, if this
        // receiver and this key are ones a negative entry may speak for.
        if (!diagnosed && absentThroughChain) {
            rtInstallAbsentEntry(ic, objRoot.get(), keyStr);
        }
    }
    return result.rawBits();
}

// `super.k` — a read of the PARENT prototype's property, with `this` as the
// receiver (ECMA-262 13.3.7.3, MakeSuperPropertyReference). For a method the
// receiver makes no difference: the value is the same function object either
// way, which is why lowering could spell `super.m` as an ordinary read for as
// long as bronze had no accessors. For a GETTER it is the whole difference —
// running it against the prototype would read the prototype's fields on every
// instance, silently.
uint64_t bronze_super_get(uint64_t protoBits, uint32_t keyIndex, uint64_t thisBits) {
    recordPropCall("bronze_super_get", keyIndex, nullptr);
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        // Keyed by return address — a super read has no IC site by design.
        // The RECEIVER (`this`) is the identity that matters for a
        // monomorphizer, not the prototype the walk starts from.
        censusRecordAccess(CensusKind::SuperGet, thisBits, keyIndex, 0, nullptr,
                           BRONZE_CENSUS_RET_ADDR(), /*hasValue=*/false, 0);
    }
    Value protoVal(protoBits);
    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("super property read with an unregistered key index");

    if (protoVal.isNull() || protoVal.isUndefined()) {
        return rtThrowTypeError("Cannot read properties of " +
                                std::string(protoVal.isNull() ? "null" : "undefined") +
                                " (reading '" + std::string(rtKeyString(keyIndex)) + "')")
            .rawBits();
    }

    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> protoRoot{protoVal};

    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
        Rooted<Value> key(Value::fromString(keyHeader));
        return rtProxyGet(protoRoot.get(), key.get(), receiver.get()).rawBits();
    }

    // A STATIC element's `super.k`: the holder is the base CONSTRUCTOR, whose
    // statics live in its property box and whose inherited statics live up
    // that box's chain (`extends` links the boxes). A static found there runs
    // with `this` as its receiver, the same as an instance read below; one
    // found nowhere on the chain is answered by the function member ladder —
    // an intrinsic's statics, `call`/`apply`/`bind`, `name` — on the holder's
    // own terms, which is what `super.of()` in a subclass of Array reaches.
    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        Rooted<Value> props{protoVal.asObject<FunctionHeader>()->properties};
        if (props.get().isObject()) {
            const uint64_t objProtoBits = rtObjectPrototype().rawBits();
            ObjectHeader* box = props.get().asObject<ObjectHeader>();
            const PropertyKey pkey = PropertyKey::forString(keyHeader);
            PropertyInfo info;
            bool found = box->shape && box->shape->lookupProperty(pkey, info);
            for (uint32_t depth = 1; !found && depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
                ObjectHeader* ancestor = box->protoAncestor(depth);
                if (!ancestor || Value::fromObject(ancestor).rawBits() == objProtoBits) break;
                found = ancestor->shape && ancestor->shape->lookupProperty(pkey, info);
            }
            if (found) {
                Rooted<Value> key(Value::fromString(keyHeader));
                return box->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr()).rawBits();
            }
        }
        return rtFunctionMember(protoRoot.get(), rtKeyString(keyIndex), keyHeader, nullptr);
    }

    if (!protoVal.isObject() ||
        !HeapKind::carriesShape(protoVal.asObject<HeapObjectHeader>()->flags)) {
        fatal("internal: super property read on a base whose prototype is not an object");
    }
    Rooted<Value> key(Value::fromString(keyHeader));
    // No inline cache: an entry describes ONE shape, and this read has two
    // objects — the holder it walks from and the receiver it runs against.
    return protoRoot.get()
        .asObject<ObjectHeader>()
        ->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr())
        .rawBits();
}

uint64_t bronze_super_elem_get(uint64_t protoBits, uint64_t keyBits, uint64_t thisBits) {
    Value protoVal(protoBits);
    if (protoVal.isNull() || protoVal.isUndefined()) {
        return rtThrowTypeError("Cannot read properties of " +
                                std::string(protoVal.isNull() ? "null" : "undefined"))
            .rawBits();
    }
    Rooted<Value> protoRoot{protoVal};
    Rooted<Value> keyRoot{Value(keyBits)};
    Rooted<Value> receiver{Value(thisBits)};

    keyRoot.set(rtToPropertyKey(keyRoot));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
        return rtProxyGet(protoRoot.get(), keyRoot.get(), receiver.get()).rawBits();
    }

    if (protoVal.isObject() && protoVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        Rooted<Value> props{protoVal.asObject<FunctionHeader>()->properties};
        if (props.get().isObject()) {
            const uint64_t objProtoBits = rtObjectPrototype().rawBits();
            ObjectHeader* box = props.get().asObject<ObjectHeader>();
            const PropertyKey pkey = PropertyKey::fromValue(keyRoot.get());
            PropertyInfo info;
            bool found = box->shape && box->shape->lookupProperty(pkey, info);
            for (uint32_t depth = 1; !found && depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
                ObjectHeader* ancestor = box->protoAncestor(depth);
                if (!ancestor || Value::fromObject(ancestor).rawBits() == objProtoBits) break;
                found = ancestor->shape && ancestor->shape->lookupProperty(pkey, info);
            }
            if (found) {
                return box->getProp(rtHeap(), keyRoot, /*ic=*/nullptr, receiver.slot_ptr()).rawBits();
            }
        }
        if (keyRoot.get().isString()) {
            StringHeader* sh = keyRoot.get().asObject<StringHeader>();
            return rtFunctionMember(protoRoot.get(), rtUtf8Chars(sh), sh, nullptr);
        }
        return Value::fromUndefined().rawBits();
    }

    if (!protoVal.isObject() ||
        !HeapKind::carriesShape(protoVal.asObject<HeapObjectHeader>()->flags)) {
        return bronze_elem_get(protoRoot.get().rawBits(), keyRoot.get().rawBits());
    }

    return protoRoot.get()
        .asObject<ObjectHeader>()
        ->getProp(rtHeap(), keyRoot, /*ic=*/nullptr, receiver.slot_ptr())
        .rawBits();
}

// `super.k = v` is a WRITE, so it lives in rt_prop_write.cpp beside the one
// answer every spelling of a store gives a refused Set.
static uint64_t elemGetHelperBody(uint64_t objBits, uint64_t idxBits) {
    recordElemCall("bronze_elem_get", objBits, idxBits);
    Value objVal(objBits);

    // 7.1.19 ToPropertyKey, for the one key kind that has to run before
    // anything else: an OBJECT, whose `toString` is user code. It is done here,
    // at the entry, and nowhere below — every branch past this point holds a raw
    // receiver header at some point, and a conversion that collects would move
    // it. The receiver is rooted across the call and the whole helper is
    // re-entered with the converted key, so the fast paths below see exactly the
    // primitive they were written for.
    if (Value(idxBits).isObject()) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> keyRoot{Value(idxBits)};
        keyRoot.set(rtToPropertyKey(keyRoot));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        return bronze_elem_get(objRoot.get().rawBits(), keyRoot.get().rawBits());
    }

    // Fast path: numeric index access on an Array or TypedArray (the common case
    // in compute and math kernels). Checked before symbol or string conversion.
    if (objVal.isObject() && idxBits <= kNumberMaxBits) {
        const double d = std::bit_cast<double>(idxBits);
        const uint32_t idx = static_cast<uint32_t>(d);
        if (d >= 0.0 && static_cast<double>(idx) == d && d <= 4294967294.0) {
            HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
            if (hdr->flags == HeapKind::Array) {
                const auto* arr = reinterpret_cast<const ArrayHeader*>(hdr);
                if (idx < arr->length) {
                    const Value v = arr->elementsData()[idx];
                    return v.isHole() ? BRONZE_ABI_UNDEFINED_BITS : v.rawBits();
                }
                return BRONZE_ABI_UNDEFINED_BITS;
            }
            if (hdr->flags == TypedArrayHeader::kFlags) {
                const auto* view = reinterpret_cast<const TypedArrayHeader*>(hdr);
                if (idx >= view->length) return BRONZE_ABI_UNDEFINED_BITS;
                const uint8_t* p = view->bytes() + static_cast<size_t>(idx) * view->bytesPerElement();
                switch (view->elementKind()) {
                    case ElementKind::Float64:
                        return Value::fromDouble(*reinterpret_cast<const double*>(p)).rawBits();
                    case ElementKind::Float32:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const float*>(p))).rawBits();
                    case ElementKind::Int32:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const int32_t*>(p))).rawBits();
                    case ElementKind::Uint32:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const uint32_t*>(p))).rawBits();
                    case ElementKind::Int16:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const int16_t*>(p))).rawBits();
                    case ElementKind::Uint16:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const uint16_t*>(p))).rawBits();
                    case ElementKind::Uint8:
                    case ElementKind::Uint8Clamped:
                        return Value::fromDouble(static_cast<double>(*p)).rawBits();
                    case ElementKind::Int8:
                        return Value::fromDouble(static_cast<double>(*reinterpret_cast<const int8_t*>(p))).rawBits();
                    // Float16 and the two BigInt kinds fall out to the funnel
                    // below: one needs a bit-pattern decode and the others
                    // allocate, and neither belongs in a switch whose whole
                    // point is loading a machine type inline.
                    default:
                        return rtTypedArrayElement(objVal, idx).rawBits();
                }
            }
        }
    }

    if (Value(idxBits).isSymbol()) {
        // Reading a property of null or undefined is the TypeError of 7.3.2
        // whatever the key is; the `fatal` below would kill the process where
        // the language throws.
        if (objVal.isNull() || objVal.isUndefined()) {
            return rtThrowTypeError("Cannot read properties of " +
                                    std::string(objVal.isNull() ? "null" : "undefined") +
                                    " (reading a symbol-keyed property)")
                .rawBits();
        }
        // A proxy first: its symbol-keyed answer is the trap's or the
        // target's (10.5.8 makes no distinction by key kind), and neither the
        // well-known-symbol dispatch nor the shape walk below knows how to ask
        // either of them.
        if (objVal.isObject() &&
            objVal.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            return rtProxyGet(objVal, Value(idxBits)).rawBits();
        }
        if (ObjectHeader* holder = rtSymbolKeyHolder(objVal)) {
            PropertyInfo info;
            if (holder->shape &&
                holder->shape->lookupProperty(
                    PropertyKey::forSymbol(Value(idxBits).asSymbol<SymbolHeader>()), info)) {
                Rooted<Value> recv{objVal};
                Rooted<Value> key{Value(idxBits)};
                Rooted<Value> holderRoot{Value::fromObject(holder)};
                return holderRoot.get()
                    .asObject<ObjectHeader>()
                    ->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr())
                    .rawBits();
            }
        }
        // Rooted because a symbol-keyed property can be an accessor, and a
        // getter is a call — and because the walk below can build an intrinsic,
        // which allocates. No inline cache: a computed site has no entry.
        //
        // The roots are taken BEFORE the well-known dispatch rather than after
        // it, and that order is load-bearing: `rtWellKnownSymbolMember` allocates
        // on two routes — a well-known symbol is created on first use and its
        // description is a heap string, and `toStringTagOf` builds its answer
        // with `rtMakeString` — so a raw receiver held across the call is a
        // pre-collection address. The first receiver to notice was a Date,
        // whose `@@toPrimitive` lives on its PROTOTYPE, so the lookup falls past
        // the own-shape probe above and through this dispatch before the walk
        // that finds it.
        Rooted<Value> recv{objVal};
        Rooted<Value> key{Value(idxBits)};
        bool handled = false;
        const Value wellKnown = rtWellKnownSymbolMember(recv, key, handled);
        if (handled) return wellKnown.rawBits();
        Rooted<Value> holderRoot{rtSymbolReadStart(recv.get())};
        // A receiver with neither own symbol-keyed storage nor a chain: an
        // array, a Map, a RegExp. Those have no own symbol-keyed property and
        // no prototype object here to inherit one from.
        if (!holderRoot.get().isObject()) return Value::fromUndefined().rawBits();
        return holderRoot.get()
            .asObject<ObjectHeader>()
            ->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr())
            .rawBits();
    }
    // The two receivers that have ELEMENTS get an index fast path, and only
    // those two: `a[i]` and `v[i]` are the whole reason this helper is not
    // just `bronze_prop_get`, and neither may walk a member table on the way
    // to a slot.
    uint32_t idx = 0;
    if (objVal.isObject()) {
        HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
        Value idxVal(idxBits);
        if (idxVal.isNumber()) {
            double d = idxVal.asNumber();
            if (d >= 0.0 && d <= 4294967294.0) {
                uint32_t u = static_cast<uint32_t>(d);
                if (static_cast<double>(u) == d) {
                    if (hdr->flags == HeapKind::Array) {
                        return reinterpret_cast<ArrayHeader*>(hdr)->getElem(u).rawBits();
                    }
                    if (hdr->flags == TypedArrayHeader::kFlags) {
                        return rtTypedArrayElement(objVal, u).rawBits();
                    }
                }
            }
        }
        if (hdr->flags == HeapKind::Array && rtValueToElementIndex(Value(idxBits), idx)) {
            return reinterpret_cast<ArrayHeader*>(hdr)->getElem(idx).rawBits();
        }
        if (hdr->flags == TypedArrayHeader::kFlags && rtValueToElementIndex(Value(idxBits), idx)) {
            // Out of range is `undefined`, not an error — 10.4.5.4 again.
            return rtTypedArrayElement(objVal, idx).rawBits();
        }
    }
    // Everything that is not an index NAMES something, and what a name means
    // cannot depend on whether the compiler knew it: `o.k` and `const s = "k";
    // o[s]` are one question. This branch used to be a second copy of
    // propGetByName's receiver-kind dispatch, and every place the two copies
    // had drifted was a silent wrong answer — `arr[s]` for "push", "length" or
    // "constructor" answered `undefined` where `arr.push` answered the method,
    // and `Math[s]` missed the namespace check that makes `Math.cbrt` a named
    // error. Delegating is what keeps them one question with one answer.
    //
    // A PRIMITIVE receiver reaches it too, which it did not before: `"abc"[i]`
    // died here as "computed index access on a non-object value" while
    // `"abc"[0]` took the name path — one operation with two answers, and the
    // reason `cases/string_index` pins both spellings.
    //
    // The computed-read cache (runtime/elem_ic.h) is consulted HERE, one line
    // above the key's conversion, and that position is the mechanism rather
    // than an ordering detail: the largest bucket in the three.js bill is a
    // NUMBER key naming a string property, and on a hit it never becomes a
    // string at all. Nothing above this point is skipped — the object-key
    // conversion, the symbol dispatch and both element paths have all had
    // their say, so what reaches here is a name, asked of a receiver whose
    // kind the probe checks for itself.
    const ElemProbe probe = elemCacheProbe(objVal, Value(idxBits));
    if (probe.hit) return probe.value.rawBits();
    {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> key{rtElemKeyAsString(Value(idxBits))};
        if (!probe.entry) {
            StringHeader* plainKey = key.get().asString<StringHeader>();
            return propGetByName(objRoot.get(), rtUtf8Chars(plainKey), plainKey, /*ic=*/nullptr);
        }
        // A single-entry site on the STACK. The walk fills it by its own rules
        // — the dictionary refusal, `chainIsCacheable`, the diagnostic claims —
        // so nothing here decides what may be cached; it copies across what the
        // walk already decided. A stack site also means a read that ends in a
        // getter, a refusal or a throw simply leaves the table alone.
        InlineCacheSite site{};
        StringHeader* keyHeader = key.get().asString<StringHeader>();
        const uint64_t result =
            propGetByName(objRoot.get(), rtUtf8Chars(keyHeader), keyHeader, &site);
        if (site.ways[0].isRealShape()) {
            // Filled only once the walk has proven the answer cacheable —
            // including ABSENCE, which reaches way 0 only through
            // `rtInstallAbsentEntry` and therefore only after the chain was
            // proven cacheable end to end, the key proven neither index-like
            // nor `length`, and no `*CheckMissingMember` diagnostic proven to
            // have claimed the receiver. This line asks none of that again; it
            // hands over what the walk decided.
            //
            // Re-read through the root: propGetByName above can allocate, and
            // the header this had before it is a pre-collection address.
            elemCacheFill(probe, key.get().asString<StringHeader>(), site);
        }
        return result;
    }
}

uint64_t bronze_elem_get(uint64_t objBits, uint64_t idxBits) {
    if (BRONZE_UNLIKELY(g_shapeCensusEnabled)) {
        // An OBJECT key is skipped: the body converts it (ToPropertyKey runs
        // user code) and re-enters this wrapper with the primitive, which is
        // the observation worth recording — one per access, keyed the way the
        // cache would have keyed it.
        if (!Value(idxBits).isObject()) {
            CensusToken tok =
                censusRecordAccess(CensusKind::ElemGet, objBits, /*keyIndex=*/0xFFFFFFFFu,
                                   idxBits, /*siteId=*/nullptr, BRONZE_CENSUS_RET_ADDR(),
                                   /*hasValue=*/false, 0);
            const uint64_t r = elemGetHelperBody(objBits, idxBits);
            censusRecordResult(tok, r);
            return r;
        }
    }
    return elemGetHelperBody(objBits, idxBits);
}

}  // extern "C"

}  // namespace bronze::runtime

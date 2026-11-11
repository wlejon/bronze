// ECMA-262 13.10.2 `instanceof` and 7.3.22 OrdinaryHasInstance, paired with a
// GC-safe thread-local `(objShape, ctorInstanceShape, ctorPropsShape)` cache
// for non-native constructors whose chain carries no custom `@@hasInstance`.
//
// Why the cache needs no GC roots or sweep hooks:
//   - Every key pointer is a non-dictionary `Shape*` owned by the thread's
//     `NonMovingArena` (dictionary shapes, whose lifetime is tied to one
//     object, are refused on both probe and fill).
//   - The arena's destructor calls `Shape::forgetLookupMemo()`, which bumps
//     `proto_epoch`, invalidating every entry before any `Shape*` address can
//     be reused.
//   - Assigning or redefining `ctor.prototype` mints or updates
//     `ctor->instance_shape` (or clears it to null), changing `ctorInstShape`.
//   - Defining `ctor[Symbol.hasInstance]` transitions `ctor->properties` away
//     from `ctorPropsShape`; defining `Base[Symbol.hasInstance]` on an
//     ancestor class or mutating any prototype link via `Object.setPrototypeOf`
//     bumps `proto_epoch`.

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/symbol.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace {

struct InstanceofCacheEntry {
    const Shape* objShape = nullptr;       // nullptr encodes a non-object (primitive) LHS
    const Shape* ctorInstShape = nullptr;  // non-null sentinel: populated iff != nullptr
    const Shape* ctorPropsShape = nullptr;
    uint64_t protoEpoch = 0;
    uint32_t realmSlot = 0;
    bool result = false;
};

constexpr uint32_t kInstanceofCacheBits = 6;
constexpr uint32_t kInstanceofCacheEntries = 1u << kInstanceofCacheBits;
thread_local InstanceofCacheEntry t_instanceofCache[kInstanceofCacheEntries];

inline uint32_t instanceofCacheIndex(const Shape* objShape, const Shape* ctorInstShape,
                                     const Shape* ctorPropsShape) noexcept {
    const uint64_t h =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(objShape)) * 0x9E3779B97F4A7C15ULL ^
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ctorInstShape)) * 0xBF58476D1CE4E5B9ULL ^
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ctorPropsShape)) * 0x94D049BB133111EBULL;
    return static_cast<uint32_t>(h >> (64 - kInstanceofCacheBits));
}

// Verifies that `obj`'s prototype walk up to `targetProto` (or the end of the
// chain) crosses only shape-carrying objects — never a Proxy link whose
// `getPrototypeOf` trap can run user code without moving `proto_epoch`.
bool objectChainCacheable(Value obj, Value targetProto) noexcept {
    if (!obj.isObject()) return true;
    const auto* firstHdr = obj.asObject<HeapObjectHeader>();
    if (!HeapKind::carriesShape(firstHdr->flags)) return false;
    const Shape* firstShape = reinterpret_cast<const ObjectHeader*>(firstHdr)->shape;
    if (!firstShape || firstShape->isDictionary()) return false;

    Value cur = obj;
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        const uint16_t kind = cur.asObject<HeapObjectHeader>()->flags;
        if (!HeapKind::carriesShape(kind)) {
            // Non-Proxy shapeless ancestors terminate `rtOrdinaryHasInstance`
            // without running traps; only Proxy is dynamic.
            return kind != HeapKind::Proxy;
        }
        const Shape* sh = cur.asObject<ObjectHeader>()->shape;
        const Value next = sh ? sh->prototypeValue() : Value::fromUndefined();
        if (!next.isObject()) return true;
        if (next.rawBits() == targetProto.rawBits()) return true;
        cur = next;
    }
    return false;
}

}  // namespace

bool rtOrdinaryHasInstance(Value ctor, Value obj) {
    if (!obj.isObject()) return false;

    Rooted<Value> objRoot{obj};
    Rooted<Value> ctorRoot{ctor};
    Rooted<Value> protoRoot{Value::fromUndefined()};

    if (ctorRoot.get().rawBits() == rtObjectNamespace().rawBits()) {
        protoRoot.set(rtObjectPrototype());
    } else {
        if (!rtIsCallableValue(ctorRoot.get())) return false;
        if (rtIsArrayConstructor(ctorRoot.get())) {
            return objRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array;
        }
        // The byte-store family and a RegExp need no arm: each carries a
        // shape, so the chain walk below answers `instanceof` for the
        // intrinsic constructors and a subclass alike (7.3.22 step 4).
        if (rtIsFunctionConstructor(ctorRoot.get())) {
            return objRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function;
        }

        if (ctorRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            // 7.3.22 step 3: `Get(C, "prototype")` — through the proxy's own
            // [[Get]] (10.5.8), so a `get` trap sees the read; step 4 makes a
            // non-object answer a TypeError rather than a false.
            Rooted<Value> key{rtMakeString("prototype")};
            protoRoot.set(rtProxyGet(ctorRoot.get(), key.get(), ctorRoot.get()));
            if (!protoRoot.get().isObject()) {
                rtThrowTypeError("Function has non-object prototype in instanceof check");
                return false;
            }
        } else {
            rtEnsureFunctionPrototype(ctorRoot);
            protoRoot.set(ctorRoot.get().asObject<FunctionHeader>()->prototype);
            if (!protoRoot.get().isObject()) return false;
        }
    }

    const uint16_t objKind = objRoot.get().asObject<HeapObjectHeader>()->flags;
    if (!HeapKind::carriesShape(objKind) && objKind != HeapKind::Proxy) {
        if (protoRoot.get().rawBits() == rtObjectPrototype().rawBits()) {
            return true;
        }
        if (objRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
            protoRoot.get().rawBits() == rtFunctionPrototypeObject().rawBits()) {
            return true;
        }
        // A SUBCLASS instance of an exotic kind does have a chain, on the box
        // holding its ordinary half (runtime/native_base.h) — so `sub
        // instanceof MySubArray` walks it here rather than answering the false
        // that every shapeless receiver used to get. An instance that is not a
        // subclass has no box and falls straight through, unchanged.
        Rooted<Value> chain{rtExoticSubclassPrototype(objRoot.get())};
        for (uint32_t depth = 0;
             chain.get().isObject() && depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
            if (protoRoot.get().rawBits() == chain.get().rawBits()) return true;
            ObjectHeader* next = chain.get().asObject<ObjectHeader>()->protoAncestor(1);
            if (!next) break;
            chain.set(Value::fromObject(next));
        }
        return false;  // arrays, functions, typed arrays
    }

    // The walk compares OBJECT IDENTITY at each link, which is why the
    // prototype has to be materialized above: a constructor whose
    // `.prototype` was never read has no object yet, and creating a
    // different one per test would answer false for its own instances.
    //
    // Step 4 is `O.[[GetPrototypeOf]]()` per link: a plain link answers from
    // its shape, a PROXY link from its `getPrototypeOf` trap (10.5.1) — which
    // is user code, so the walker is a root — and any other kind ends the
    // walk, its members being answered beside the value rather than found on
    // an object the chain could reach.
    Rooted<Value> cur{objRoot.get()};
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        Value next = Value::fromUndefined();
        const uint16_t kind = cur.get().asObject<HeapObjectHeader>()->flags;
        if (HeapKind::carriesShape(kind)) {
            Shape* shape = cur.get().asObject<ObjectHeader>()->shape;
            next = shape ? shape->prototypeValue() : Value::fromUndefined();
        } else if (kind == HeapKind::Proxy) {
            next = rtProxyGetPrototypeOf(cur.get());
        } else {
            return false;
        }
        if (!next.isObject()) return false;
        if (protoRoot.get().rawBits() == next.rawBits()) return true;
        cur.set(next);
    }
    fatal("prototype chain too deep (a cycle?)");
}

extern "C" bool bronze_instanceof(uint64_t objBits, uint64_t ctorBits) {
    recordHelperCall("bronze_instanceof");
    const Value ctorVal(ctorBits);
    const Value objVal(objBits);

    // Zero-allocation direct-mapped probe before constructing shadow-stack roots.
    if (ctorVal.isObject() && ctorVal.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        const auto* ctorFn = ctorVal.asObject<FunctionHeader>();
        const Shape* ctorInst = ctorFn->instance_shape;
        if (!ctorFn->isNativeCode() && ctorInst != nullptr && !ctorInst->isDictionary() &&
            ctorFn->prototype.isObject()) {
            bool propsOk = false;
            const Shape* ctorProps = nullptr;
            if (ctorFn->properties.isUndefined()) {
                propsOk = ctorFn->parent.isUndefined();
            } else if (ctorFn->properties.isObject()) {
                ctorProps = ctorFn->properties.asObject<ObjectHeader>()->shape;
                propsOk = (ctorProps != nullptr && !ctorProps->isDictionary());
            }
            if (propsOk) {
                bool objOk = false;
                const Shape* objShape = nullptr;
                if (!objVal.isObject()) {
                    objOk = true;
                } else {
                    const auto* objHdr = objVal.asObject<HeapObjectHeader>();
                    if (HeapKind::carriesShape(objHdr->flags)) {
                        objShape = reinterpret_cast<const ObjectHeader*>(objHdr)->shape;
                        objOk = (objShape != nullptr && !objShape->isDictionary());
                    }
                }
                if (objOk) {
                    const bronze_tls_block* tls = rtTls();
                    const uint64_t epoch = tls->proto_epoch;
                    const uint32_t realm = static_cast<uint32_t>(tls->realm_slot);
                    const InstanceofCacheEntry& e =
                        t_instanceofCache[instanceofCacheIndex(objShape, ctorInst, ctorProps)];
                    if (e.ctorInstShape == ctorInst && e.objShape == objShape &&
                        e.ctorPropsShape == ctorProps && e.protoEpoch == epoch &&
                        e.realmSlot == realm) {
                        return e.result;
                    }
                }
            }
        }
    }

    Rooted<Value> objRoot{objVal};
    Rooted<Value> ctorRoot{ctorVal};

    if (!ctorRoot.get().isObject()) {
        rtThrowTypeError("Right-hand side of 'instanceof' is not an object");
        return false;
    }

    // Fast path for ordinary Function constructors whose chain has no custom
    // `[Symbol.hasInstance]` property and ends at `%Function.prototype%`
    // (whose `[Symbol.hasInstance]` is non-writable and non-configurable).
    if (ctorRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        const FunctionHeader* cur = ctorRoot.get().asObject<FunctionHeader>();
        const PropertyKey hasInstProp = PropertyKey::forSymbol(rtSymbolHasInstance());
        bool ancestorsMarked = true;
        for (uint32_t depth = 0; cur != nullptr && depth < 16; ++depth) {
            if (cur->properties.isObject()) {
                PropertyInfo info;
                Shape* sh = cur->properties.asObject<ObjectHeader>()->shape;
                if (sh && sh->lookupProperty(hasInstProp, info)) break;
                if (depth > 0 && (!sh || sh->isDictionary() || !sh->used_as_prototype)) {
                    ancestorsMarked = false;
                }
            } else if (depth > 0) {
                ancestorsMarked = false;
            }
            if (cur->parent.isUndefined()) {
                const bool answer = rtOrdinaryHasInstance(ctorRoot.get(), objRoot.get());
                const auto* liveCtor = ctorRoot.get().asObject<FunctionHeader>();
                const Shape* fillInst = liveCtor->instance_shape;
                if (ancestorsMarked && !liveCtor->isNativeCode() && fillInst != nullptr &&
                    !fillInst->isDictionary() && liveCtor->prototype.isObject() &&
                    fillInst->prototypeValue().rawBits() == liveCtor->prototype.rawBits() &&
                    objectChainCacheable(objRoot.get(), liveCtor->prototype)) {
                    bool fillPropsOk = false;
                    const Shape* fillProps = nullptr;
                    if (liveCtor->properties.isUndefined()) {
                        fillPropsOk = liveCtor->parent.isUndefined();
                    } else if (liveCtor->properties.isObject()) {
                        fillProps = liveCtor->properties.asObject<ObjectHeader>()->shape;
                        fillPropsOk = (fillProps != nullptr && !fillProps->isDictionary());
                    }
                    if (fillPropsOk) {
                        const Shape* fillObjShape =
                            objRoot.get().isObject()
                                ? objRoot.get().asObject<ObjectHeader>()->shape
                                : nullptr;
                        const bronze_tls_block* tls = rtTls();
                        InstanceofCacheEntry& dst = t_instanceofCache[instanceofCacheIndex(
                            fillObjShape, fillInst, fillProps)];
                        dst.objShape = fillObjShape;
                        dst.ctorInstShape = fillInst;
                        dst.ctorPropsShape = fillProps;
                        dst.protoEpoch = tls->proto_epoch;
                        dst.realmSlot = static_cast<uint32_t>(tls->realm_slot);
                        dst.result = answer;
                    }
                }
                return answer;
            }
            if (cur->parent.isObject() &&
                cur->parent.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                cur = cur->parent.asObject<FunctionHeader>();
            } else {
                break;
            }
        }
    }

    Rooted<Value> hasInstKey{Value::fromSymbol(rtSymbolHasInstance())};
    Value handler(bronze_elem_get(ctorRoot.get().rawBits(), hasInstKey.get().rawBits()));

    if (!handler.isNull() && !handler.isUndefined()) {
        if (handler.isObject() &&
            handler.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
            handler.asObject<FunctionHeader>()->code == rtFunctionHasInstanceBuiltin) {
            return rtOrdinaryHasInstance(ctorRoot.get(), objRoot.get());
        }
        if (!rtIsCallableValue(handler)) {
            rtThrowTypeError("Symbol.hasInstance is not a function");
            return false;
        }
        uint64_t arg = objRoot.get().rawBits();
        uint64_t res = bronze_dynamic_call(handler.rawBits(), ctorRoot.get().rawBits(), 1, &arg);
        return bronze_truthy(res);
    }

    if (ctorRoot.get().rawBits() == rtObjectNamespace().rawBits()) {
        return rtOrdinaryHasInstance(ctorRoot.get(), objRoot.get());
    }

    if (!rtIsCallableValue(ctorRoot.get())) {
        rtThrowTypeError("Right-hand side of 'instanceof' is not callable");
        return false;
    }
    return rtOrdinaryHasInstance(ctorRoot.get(), objRoot.get());
}

}  // namespace bronze::runtime

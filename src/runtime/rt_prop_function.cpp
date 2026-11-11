// Property READS on a FUNCTION receiver — a class constructor, an ordinary
// function, one of the interned intrinsic singletons. Its own translation unit
// for the reason rt_prop.cpp is split by receiver kind at all: this is one
// kind, and it is the one whose answer comes from FOUR places in a fixed order
// — the global-constructor table, the `prototype` slot, the function's own
// statics object (and the base classes' it inherits through `extends`), then
// %Function.prototype%'s own statics box and %Object.prototype%.
//
// That order is the file's whole content and every step of it is load-bearing;
// each one carries the clause it implements. The one thing to know before
// reading it is that a function's `static` members do not live in the
// FunctionHeader. They live in a side object hanging off `properties`, with its
// own shape and its own prototype chain — the chain `extends` links to the base
// class's statics. `C.DEFAULT_UP` is therefore an own property of an object
// that is not the receiver, which is what `installStaticsCacheEntry` below has
// to say out loud to the inline cache.

#include <cstdlib>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/accessor.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_prop_mega.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/value.h"

namespace bronze::runtime {

// May a shape-keyed inline-cache entry describe this (function receiver,
// statics box) pair at all? Two fills ask it: the property READ's
// `installStaticsCacheEntry` below, and the method call's
// `latchFunctionStaticsMethodIc` (rt_method_call.cpp), which caches the same
// box's shape and slot so a call's callee lookup hits where a read's does. One
// function rather than a copy each, because every refusal here is about the
// RECEIVER and the BOX and not about what the site goes on to do with the slot
// — the two askers differ only in that.
//
// Three refusals, and each one is a correctness condition rather than a
// heuristic:
//
//   - An INTRINSIC CONSTRUCTOR is never cached. `rtGlobalConstructorMember`
//     ran ahead of the statics lookup and won for every name it knows, so a
//     shape-keyed entry could answer where the table is supposed to. The
//     collision is reachable, not theoretical: `class C { static keys = 1 }`
//     gives C's statics the shape `{keys}`, and a first `Object.keys = 9`
//     would give %Object%'s statics the same interned shape pointer — after
//     which one warm site would answer 9 where the language says the builtin.
//   - An ACCESSOR is never cached — the caller's own gate, since only it knows
//     whether the property it found is one. A `static get` sees the CLASS as
//     its receiver, not the side object the getter is stored in (which is why
//     the read below hands `getProp` the function's own slot). Generated
//     code's inline arms hold the box, so they would dispatch the getter with
//     the wrong receiver; both stay on the helper instead.
//   - A DICTIONARY shape is never cached unless it is frozen
//     (Shape::readCacheable), on the same terms as the plain-object
//     path: its slots are not shape-indexed and its shape is private to one
//     object, so an entry naming it goes stale on the next delete.
//
// A statics shape and a plain object's shape are drawn from the same arena and
// CAN be the same pointer — `rtEnsureFunctionProperties` starts the box from
// `rtPlainObjectShape()`, the very root `{}` starts from. That collision is
// harmless and is worth saying why: a shape IS a key-to-slot map and nothing
// else, so a plain receiver that matches such an entry finds the site's key at
// the same slot in ITSELF, which is the answer it should have had. What an
// entry must not do is survive into a case where the answer does NOT come from
// a key-to-slot map, and the refusals above are exactly those cases.
bool rtStaticsBoxCacheable(Value fnVal, const ObjectHeader* box) noexcept {
    if (box == nullptr || box->shape == nullptr || !box->shape->readCacheable()) return false;
    // Refuse only constructors whose `kCtors` row answers static members or
    // unimplemented-member diagnostics ahead of `properties` (`Array`,
    // `String`, `Proxy`). `Number`, `Boolean`, `Object`, typed-array
    // constructors and user classes keep their statics in `properties` itself.
    if (rtIntrinsicConstructorHasTableStatics(fnVal)) return false;
    return !censusFillsSuppressed();
}

bool rtStaticsArmServes(Value fnVal) noexcept {
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return false;
    const uint8_t flags = fnVal.asObject<FunctionHeader>()->function_flags;
    return (flags & BRONZE_ABI_FN_FLAG_NATIVE) == 0 && (flags & BRONZE_ABI_FN_FLAG_PROTOTYPE) != 0;
}

namespace {

bool tryFastBoxlessFunctionMember(Value fnVal, const FunctionHeader* fn,
                                  StringHeader* keyHeader, uint64_t& out) noexcept {
    if (keyHeader == nullptr || fn->properties.isObject()) return false;
    if (fn->isNativeCode() && rtIntrinsicConstructorHasTableStatics(fnVal)) return false;
    if (keyHeader->equalsASCII("length")) {
        out = Value::fromDouble(fn->name ? static_cast<double>(fn->length) : 0.0).rawBits();
        return true;
    }
    if (fn->name != nullptr && keyHeader->equalsASCII("name")) {
        out = rtKeyAsValue(fn->name).rawBits();
        return true;
    }
    if (fn->parent.isUndefined() && !fn->isGeneratorFunction() && !fn->isAsyncFunction() &&
        (keyHeader->equalsASCII("apply") || keyHeader->equalsASCII("call") ||
         keyHeader->equalsASCII("bind") || keyHeader->equalsASCII("toString"))) {
        const Value fpVal = rtFunctionPrototypeIfBuilt();
        if (fpVal.isObject()) {
            const Value fpBoxVal = fpVal.asObject<FunctionHeader>()->properties;
            if (fpBoxVal.isObject()) {
                auto* fpObj = fpBoxVal.asObject<ObjectHeader>();
                PropertyInfo info;
                if (fpObj->shape && !fpObj->shape->isDictionary() &&
                    fpObj->shape->lookupProperty(PropertyKey::forString(keyHeader), info) &&
                    !info.accessor) {
                    out = fpObj->getSlot(info.slot).rawBits();
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

bool rtFunctionCachedMember(Value fnVal, InlineCacheSite* site, uint32_t keyIndex, uint64_t& out) {
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const auto* fn = fnVal.asObject<FunctionHeader>();
    const Value box = fn->properties;
    if (!box.isObject()) {
        if (keyIndex != BRONZE_ABI_KEY_NONE) {
            return tryFastBoxlessFunctionMember(fnVal, fn, rtKeyHeader(keyIndex), out);
        }
        return false;
    }
    if (site == nullptr || box.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return false;
    if (fn->isNativeCode() && rtIntrinsicConstructorHasTableStatics(fnVal)) return false;
    ObjectHeader* boxObj = box.asObject<ObjectHeader>();
    InlineCache* ic = site->find(boxObj->shape);
    if (ic == nullptr && keyIndex != BRONZE_ABI_KEY_NONE &&
        site->ways[BRONZE_ABI_IC_WAYS - 1].isRealShape()) {
        ic = rtMegaPropProbe(boxObj->shape, keyIndex);
    }
    if (ic == nullptr || !ic->describesForFunction(boxObj->shape)) return false;
    ObjectHeader* holder = boxObj;
    if (ic->isStatics()) {
        if (!rtStaticsArmServes(fnVal)) return false;
        bool crossedDictionary = false;
        holder = boxObj->cachedProtoHolder(ic->staticsDepth(), crossedDictionary);
        if (holder == nullptr) return false;
    }
    if (!ic->isAccessor()) {
        out = holder->getSlot(ic->cached_slot).rawBits();
        return true;
    }
    // A getter sees the function, not the box it is kept in.
    Rooted<Value> self{fnVal};
    Rooted<Value> getter{holder->getSlot(ic->cached_slot)};
    out = callGetter(getter.get(), self).rawBits();
    return true;
}

namespace {

// Every link `depth` up from a statics box is a plain object of a
// non-dictionary shape marked as a prototype, so an add to any of them bumps
// the epoch an inherited entry is filled at (InlineCache::describesForFunction).
bool statsChainMarked(ObjectHeader* box, uint32_t depth) {
    ObjectHeader* cur = box;
    for (uint32_t i = 0; i < depth; ++i) {
        if (!cur->shape) return false;
        const Value proto = cur->shape->prototypeValue();
        if (!proto.isObject() || proto.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return false;
        cur = proto.asObject<ObjectHeader>();
        if (!cur->shape || cur->shape->isDictionary() || !cur->shape->used_as_prototype) return false;
    }
    return true;
}

// Teach this site that the answer for a FUNCTION receiver is an own data
// property of the receiver's STATICS object, so the next read hits inline
// instead of walking this whole file again.
//
// What goes in the entry is the statics object's shape and slot, NOT the
// receiver's — a function has no shape of its own. Generated code holds up its
// end by loading `properties` out of the FunctionHeader and scanning way 0
// against THAT object's shape (llvm_prop_get.cpp), so the two sides describe
// the same object or neither does.
//
// An own ACCESSOR of the box is filled too, as the accessor entry a plain
// object's own accessor gets: every reader of it calls the getter with the
// RECEIVER as `this` (the function, for a function receiver), never the box.
//
// A static INHERITED through `extends` (`depth` links up the box chain, every
// link a base class's box) is filled with BRONZE_ABI_IC_DEPTH_STATICS_FLAG,
// which only the function arms take (InlineCache::describesForFunction): a
// plain object's chain entry at the same shape would name %Object.prototype%,
// which is not on a function's path to its members, and the flag keeps the
// two apart. Only for a function the arms serve (staticsArmServes).
void installStaticsCacheEntry(Value fnVal, InlineCacheSite* site, ObjectHeader* propsObj,
                              const PropertyInfo& found, uint32_t depth) {
    if (site == nullptr) return;
    if (!rtStaticsBoxCacheable(fnVal, propsObj)) return;
    if (depth > 0 && !rtStaticsArmServes(fnVal)) return;
    const uint32_t word = depth == 0 ? 0u : (depth | BRONZE_ABI_IC_DEPTH_STATICS_FLAG);
    if (InlineCache* into = site->slotForInstall(propsObj->shape)) {
        if (found.accessor) {
            into->fillAccessor(propsObj->shape, found.slot, word);
        } else {
            into->fill(propsObj->shape, found.slot, word);
        }
    }
}

// The keys a function's ladder answers between its own box and the boxes
// `extends` linked in: an inherited entry for one would skip that answer.
bool keyAnsweredBeforeInheritedStatics(const StringHeader* keyHeader) {
    return keyHeader->equalsASCII("prototype") || keyHeader->equalsASCII("length") ||
           keyHeader->equalsASCII("name") || keyHeader->equalsASCII("constructor");
}

uint64_t functionMemberImpl(Value objVal, const std::string* optKeyStr, StringHeader* keyHeader,
                            InlineCacheSite* site) {
    // Non-native functions never appear in `kCtors`, never have
    // `statics_materialized`, and are never the `%Function%` constructor, so a
    // materialized `prototype` (or an arrow/method/async's permanent absence of
    // one) answers immediately without rooting or UTF-8 string conversion.
    if (const auto* fastFn = objVal.asObject<FunctionHeader>(); !fastFn->isNativeCode() &&
                                                                keyHeader->equalsASCII("prototype")) {
        if (!fastFn->hasPrototypeProperty()) {
            return Value::fromUndefined().rawBits();
        }
        if (fastFn->prototype.isObject()) {
            return fastFn->prototype.rawBits();
        }
    }
    if (uint64_t fastOut = 0;
        tryFastBoxlessFunctionMember(objVal, objVal.asObject<FunctionHeader>(), keyHeader, fastOut)) {
        return fastOut;
    }

    std::string lazyKeyStr;
    auto keyText = [&]() -> const std::string& {
        if (optKeyStr) return *optKeyStr;
        if (lazyKeyStr.empty() && keyHeader->getLength() != 0) {
            lazyKeyStr = rtUtf8Chars(keyHeader);
        }
        return lazyKeyStr;
    };

    // Rooted for the same reason the array branch is: the tail below walks
    // `Object.prototype`, and everything between here and there allocates.
    Rooted<Value> recv{objVal};
    // A GLOBAL CONSTRUCTOR's statics come first, ahead of the `prototype`
    // slot below. That order is the whole point: a FunctionHeader answers
    // `prototype` from a slot it creates on demand, so `Array.prototype`
    // would read as an empty object — a silent lie about an intrinsic
    // bronze does not have, and one a program could install a method on
    // that nothing would ever find.
    //
    // Once the program has written to such a constructor's statics they live
    // in its box (rtPrepareFunctionOwnWrite), and the table stops answering
    // them. A name the program put in the box is then found by the box block
    // below BEFORE the table's unimplemented-member diagnosis can refuse it —
    // a polyfill for a member bronze has not built is the program's to add.
    // Every entry in `kCtors` is a native function, so source functions skip
    // both the box pre-probe and the table lookup.
    if (recv.get().asObject<FunctionHeader>()->isNativeCode()) {
        bool boxFirst = false;
        if (recv.get().asObject<FunctionHeader>()->statics_materialized) {
            const Value box = recv.get().asObject<FunctionHeader>()->properties;
            PropertyInfo own;
            boxFirst = box.isObject() && box.asObject<ObjectHeader>()->shape &&
                       box.asObject<ObjectHeader>()->shape->lookupProperty(
                           PropertyKey::forString(keyHeader), own);
        }
        if (Value ctorMember;
            !boxFirst && rtGlobalConstructorMember(recv.get(), keyText(), ctorMember)) {
            return ctorMember.rawBits();
        }
    }
    // `prototype` lives in its own slot; every other own property lives in
    // the function's property object and is found through ITS prototype
    // chain, which `extends` linked to the base class's. Reading
    // `prototype` first is what keeps `call`, `bind` and `name` answered
    // or diagnosed rather than read as undefined.
    if (keyHeader->equalsASCII("prototype")) {
        if (rtIsFunctionConstructor(recv.get())) {
            return rtFunctionPrototypeObject().rawBits();
        }
        // Every other intrinsic constructor (%TypedArray%, the twelve views,
        // the two buffers, DataView, the collections) writes its real
        // prototype object into the slot when it is built, so the read below
        // is exact for it too.
        //
        // An arrow, a method, an accessor and an async function have NO
        // `prototype` property at all (15.3.4, 15.4.4, 15.8.4 build them
        // with no CreateMethodProperty step for it), so the read is
        // `undefined` and — because the slot stays empty — nothing later
        // hands `new` an instance prototype either.
        if (!recv.get().asObject<FunctionHeader>()->hasPrototypeProperty()) {
            return Value::fromUndefined().rawBits();
        }
        rtEnsureFunctionPrototype(recv);
        return recv.get().asObject<FunctionHeader>()->prototype.rawBits();
    }
    // ROOTED for the whole branch, not just for the own-property block: the
    // box is read TWICE, once for the function's own statics here and again
    // for the ones `extends` linked in, with the entire miss ladder in
    // between. Every probe in that ladder is meant to be allocation-free
    // for a receiver that is not its own, but "meant to" is not a
    // guarantee a compiler checks, and one probe that builds an intrinsic
    // on first use is enough to relocate this box — after which the second
    // read walks a retired header and either misses a static that is there
    // or dereferences a garbage shape. Rooting it costs one shadow-stack
    // slot and makes the ladder's discipline unnecessary rather than
    // load-bearing.
    Rooted<Value> props{recv.get().asObject<FunctionHeader>()->properties};
    if (props.get().isObject()) {
        // The receiver a `static get` sees is the CLASS, not the side
        // object its statics are kept in — which is the whole reason
        // getProp takes a receiver at all.
        ObjectHeader* propsObj = props.get().asObject<ObjectHeader>();
        PropertyInfo own;
        if (propsObj->shape &&
            propsObj->shape->lookupProperty(
                PropertyKey::forString(keyHeader), own)) {
            // Before the read, because the read can run a getter and a getter
            // can collect: `propsObj` and `own` are both dead after it, and the
            // entry is about the shape they name now.
            installStaticsCacheEntry(recv.get(), site, propsObj, own, 0);
            Rooted<Value> key(Value::fromString(keyHeader));
            return propsObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
        }
    }
    // `length` and `name`, the two own data properties 10.2.10 and 10.2.9
    // give every function object. Both are non-writable and non-enumerable,
    // and both live in the header rather than in the statics table above:
    // they are created by OrdinaryFunctionCreate before any `static` can be
    // written, and a program cannot overwrite either (rt_prop_write.cpp
    // refuses the assignment).
    //
    // They are read AFTER the statics all the same, and that order is the
    // language's: `class C { static name() {} }` DEFINES a `name` property
    // over the one 15.7.14 step 15 had just given the constructor, so the
    // method wins. An assignment could not have put anything there, so the
    // only thing this order can find first is a definition that really did
    // replace the property.
    if (const FunctionHeader* fn = recv.get().asObject<FunctionHeader>(); fn->name) {
        if (keyHeader->equalsASCII("length")) return Value::fromDouble(fn->length).rawBits();
        if (keyHeader->equalsASCII("name")) return rtKeyAsValue(fn->name).rawBits();
    } else if (keyHeader->equalsASCII("name")) {
        return rtMakeString("").rawBits();
    } else if (keyHeader->equalsASCII("length")) {
        return Value::fromDouble(0.0).rawBits();
    }
    if (recv.get().asObject<FunctionHeader>()->isNativeCode()) {
        rtSymbolCheckMissingMember(recv.get(), keyText());
        // `Object` is a function object too (20.1.1), so its unimplemented-member
        // table is consulted on THIS miss path and not on the plain object one
        // below. Without this line a name 20.1.2 defines and bronze has not
        // built read `undefined` from the moment `Object` stopped being a
        // namespace.
        rtObjectCheckMissingMember(recv.get(), keyText());
    }
    // `constructor` for the three forms that do not inherit it from
    // %Function.prototype%: a generator function's is %GeneratorFunction%,
    // not `Function`, and the table below cannot tell them apart because
    // it is asked by KEY and never sees the receiver. Answered `undefined`
    // for an ordinary function, which falls straight through to it.
    if (keyHeader->equalsASCII("constructor")) {
        if (Value ctor = rtFunctionKindConstructor(recv.get()); !ctor.isUndefined()) {
            return ctor.rawBits();
        }
    }
    // The statics `extends` linked in, BEFORE %Function.prototype%'s own
    // members below: a base class's `static call()` is nearer on the chain
    // than 20.2.3.3, so a derived class reads the static — the ordinary
    // shadowing rule, and the reason the cheaper lookup is not made first.
    if (props.get().isObject()) {
        // The chain's end BEFORE the pointer that walks toward it:
        // `rtObjectPrototype` builds %Object.prototype% on first use, so a
        // `propsObj` read out first would be the address the box had before
        // that allocation. Same statement, opposite order, and only one of
        // the two orders survives a collection here.
        const uint64_t objProtoBits = rtObjectPrototype().rawBits();
        ObjectHeader* propsObj = props.get().asObject<ObjectHeader>();
        for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
            ObjectHeader* ancestor = propsObj->protoAncestor(depth);
            if (!ancestor || Value::fromObject(ancestor).rawBits() == objProtoBits) break;
            PropertyInfo inherited;
            if (ancestor->shape &&
                ancestor->shape->lookupProperty(PropertyKey::forString(keyHeader), inherited)) {
                // The ladder's steps between the own box and here answered
                // nothing for this key (they returned above), and for any
                // other function the arms serve they answer by key alone, so
                // the entry is the answer for every box at this shape while
                // the epoch holds.
                if (!keyAnsweredBeforeInheritedStatics(keyHeader) &&
                    statsChainMarked(propsObj, depth)) {
                    installStaticsCacheEntry(recv.get(), site, propsObj, inherited, depth);
                }
                Rooted<Value> key(Value::fromString(keyHeader));
                return propsObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
            }
        }
    }
    // %Function.prototype%'s own members (20.2.3): `call`, `apply`, `bind`,
    // `toString` and `constructor`, held in ITS statics box, which is the
    // one box every function receiver's chain reaches whether or not the
    // receiver has a box of its own — a closure that was never assigned a
    // static has none, and must still read `call`. The receiver is handed
    // to the read so an accessor installed there would see the function and
    // not the box. `rtFunctionPrototypeObject` builds the intrinsics on
    // first use, which is why the box is taken through a root afterwards.
    {
        Rooted<Value> fpBox{rtFunctionPrototypeObject().asObject<FunctionHeader>()->properties};
        ObjectHeader* fpObj = fpBox.get().asObject<ObjectHeader>();
        PropertyInfo inherited;
        if (fpObj->shape->lookupProperty(PropertyKey::forString(keyHeader), inherited)) {
            Rooted<Value> key(Value::fromString(keyHeader));
            return fpObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
        }
    }
    // `Function.prototype` has had its say, so what is left is the object
    // above it. That step is what makes `f.hasOwnProperty` a function rather
    // than `undefined`.
    return rtObjectProtoMember(recv, keyText()).rawBits();
}

}  // namespace

uint64_t rtFunctionMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                          InlineCacheSite* site) {
    return functionMemberImpl(objVal, &keyStr, keyHeader, site);
}

uint64_t rtFunctionMember(Value objVal, StringHeader* keyHeader, InlineCacheSite* site) {
    return functionMemberImpl(objVal, nullptr, keyHeader, site);
}

}  // namespace bronze::runtime

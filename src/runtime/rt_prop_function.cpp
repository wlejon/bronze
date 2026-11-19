// Property READS on a FUNCTION receiver — a class constructor, an ordinary
// function, one of the interned intrinsic singletons. Its own translation unit
// for the reason rt_prop.cpp is split by receiver kind at all: this is one
// kind, and it is the one whose answer comes from FOUR places in a fixed order
// — the global-constructor table, the `prototype` slot, the function's own
// statics (and the base classes' it inherits through `extends`), then
// %Function.prototype%'s own members and %Object.prototype%.
//
// That order is the file's whole content and every step of it is load-bearing;
// each one carries the clause it implements. The one thing to know before
// reading it is that a function's `static` members are slots of the function
// itself — its shape and overflow block, as a plain object's properties are
// (fn.h) — but its shape's ROOT names the chain those statics inherit
// through, which `extends` roots at the base class, and not the function's
// [[Prototype]]. So an own static is an ordinary own-property cache entry, and
// an inherited one is a STATICS entry (`installStaticsCacheEntry` below) that
// only the function arms take.

#include <cstdlib>
#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/accessor.h"
#include "runtime/fatal.h"
#include "runtime/intrinsic_additions.h"
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

// May a shape-keyed inline-cache entry describe this function receiver's
// statics at all? Two fills ask it: the property READ's
// `installStaticsCacheEntry` below, and the method call's
// `latchFunctionStaticsMethodIc` (rt_method_call.cpp), which caches the same
// shape and slot so a call's callee lookup hits where a read's does. One
// function rather than a copy each, because every refusal here is about the
// RECEIVER and not about what the site goes on to do with the slot — the two
// askers differ only in that.
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
//     its receiver, not the base class an inherited one is stored in (which is
//     why the reads below hand `getProp` the receiver). An own accessor is
//     filled as a plain object's own accessor is; an inherited one stays on
//     the helper.
//   - A DICTIONARY shape is never cached unless it is frozen
//     (Shape::readCacheable), on the same terms as the plain-object
//     path: its slots are not shape-indexed and its shape is private to one
//     object, so an entry naming it goes stale on the next delete.
//
// A function's shape is drawn from a BARE root (no inline Values), which no
// plain object ever has, so no plain object's entry can name it; an array at
// the same bare root (one over the same prototype) holds its named properties
// the same way, and a shape is a key-to-slot map and nothing else, so an own
// entry filled for one answers for the other with the slot its key really is
// in. What an entry must not do is survive into a case where the answer does
// NOT come from a key-to-slot map, and the refusals above are exactly those
// cases. (An intrinsic constructor with table statics may share a shape with
// a function whose entry a site holds: a key that shape has is one the
// constructor holds itself, and an own static answers ahead of its table.)
bool rtStaticsCacheable(Value fnVal, const ObjectHeader* self) noexcept {
    if (self == nullptr || self->shape == nullptr || !self->shape->readCacheable()) return false;
    // Refuse only constructors whose `kCtors` row answers static members or
    // unimplemented-member diagnostics ahead of their own statics (`Array`,
    // `String`, `Proxy`). `Number`, `Boolean`, `Object`, typed-array
    // constructors and user classes keep their statics in their own slots.
    if (rtIntrinsicConstructorHasTableStatics(fnVal)) return false;
    return !censusFillsSuppressed();
}

ObjectHeader* rtStaticsAncestor(ObjectHeader* fnObj, uint32_t depth, bool* crossedDictionary) {
    if (crossedDictionary) *crossedDictionary = false;
    ObjectHeader* cur = fnObj;
    for (uint32_t i = 0; i < depth; ++i) {
        if (!cur->shape) return nullptr;
        const Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return nullptr;
        const uint16_t kind = proto.asObject<HeapObjectHeader>()->flags;
        if (kind != HeapKind::Function && !HeapKind::shapeNamesPrototype(kind)) return nullptr;
        cur = proto.asObject<ObjectHeader>();
        if (crossedDictionary && cur->shape && !cur->shape->readCacheable()) {
            *crossedDictionary = true;
            return nullptr;
        }
    }
    return cur;
}

bool rtFunctionStaticsGet(Rooted<Value>& fn, Rooted<Value>& key, Rooted<Value>& receiver, Value& out) {
    const PropertyKey pkey = PropertyKey::fromValue(key.get());
    ObjectHeader* fnObj = fn.get().asObject<ObjectHeader>();
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        ObjectHeader* holder = depth == 0 ? fnObj : rtStaticsAncestor(fnObj, depth);
        if (!holder) return false;
        PropertyInfo info;
        if (holder->shape && holder->shape->lookupProperty(pkey, info)) {
            out = holder->getProp(rtHeap(), key, /*ic=*/nullptr, receiver.slot_ptr());
            return true;
        }
    }
    return false;
}

bool rtStaticsArmServes(Value fnVal) noexcept {
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return false;
    const uint8_t flags = fnVal.asObject<FunctionHeader>()->function_flags;
    return (flags & BRONZE_ABI_FN_FLAG_NATIVE) == 0 && (flags & BRONZE_ABI_FN_FLAG_PROTOTYPE) != 0;
}

namespace {

// The statics of an intrinsic function after lockdown: the walk read the
// function's members and froze them hardened. Its keys and its statics chain
// are fixed for good.
bool hardenedStatics(const FunctionHeader* fn) noexcept {
    const Shape* shape = fn->shape;
    return shape != nullptr && shape->dict != nullptr && shape->dict->hardened &&
           shape->dict->level == IntegrityLevel::Frozen;
}

// A function's member without the ladder: `length` and `name` of a function
// with no statics, and `call` / `apply` / `bind` / `toString` read off
// %Function.prototype%. Lockdown must not cost these: it freezes the statics
// of the intrinsic functions that have any and %Function.prototype%'s into
// dictionaries, and a frozen dictionary is as fixed a key-to-slot map as a
// shape (Shape::readCacheable). So hardened statics that do not hold the key
// are passed like none, and the prototype's are read frozen or not; before,
// every `f.call(...)` of a locked-down program walked the whole ladder (1,800
// instructions a call).
bool tryFastBoxlessFunctionMember(Value fnVal, const FunctionHeader* fn,
                                  StringHeader* keyHeader, uint64_t& out) noexcept {
    if (keyHeader == nullptr) return false;
    const bool boxed = fn->hasOwnStatics();
    if (boxed && !hardenedStatics(fn)) return false;
    if (fn->isNativeCode() && rtIntrinsicConstructorHasTableStatics(fnVal)) return false;
    if (!boxed) {
        if (keyHeader->equalsASCII("length")) {
            out = Value::fromDouble(fn->name ? static_cast<double>(fn->length) : 0.0).rawBits();
            return true;
        }
        if (fn->name != nullptr && keyHeader->equalsASCII("name")) {
            out = rtKeyAsValue(fn->name).rawBits();
            return true;
        }
    }
    if (fn->parent.isUndefined() && !fn->isGeneratorFunction() && !fn->isAsyncFunction() &&
        (keyHeader->equalsASCII("apply") || keyHeader->equalsASCII("call") ||
         keyHeader->equalsASCII("bind") || keyHeader->equalsASCII("toString"))) {
        if (boxed) {
            PropertyInfo own;
            if (fn->shape->lookupProperty(PropertyKey::forString(keyHeader), own)) {
                return false;
            }
        }
        const Value fpVal = rtFunctionPrototypeIfBuilt();
        if (fpVal.isObject()) {
            const ObjectHeader* fpObj = fpVal.asObject<FunctionHeader>()->object();
            PropertyInfo info;
            if (fpObj->shape && fpObj->shape->readCacheable() &&
                fpObj->shape->lookupProperty(PropertyKey::forString(keyHeader), info) &&
                !info.accessor) {
                out = fpObj->getSlot(info.slot).rawBits();
                return true;
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
    auto* fn = fnVal.asObject<FunctionHeader>();
    if (!fn->hasOwnStatics() || hardenedStatics(fn)) {
        if (keyIndex != BRONZE_ABI_KEY_NONE &&
            tryFastBoxlessFunctionMember(fnVal, fn, rtKeyHeader(keyIndex), out)) {
            return true;
        }
    }
    if (site == nullptr) return false;
    if (fn->isNativeCode() && rtIntrinsicConstructorHasTableStatics(fnVal)) return false;
    ObjectHeader* fnObj = fn->object();
    InlineCache* ic = site->find(fnObj->shape);
    if (ic == nullptr && keyIndex != BRONZE_ABI_KEY_NONE &&
        site->ways[BRONZE_ABI_IC_WAYS - 1].isRealShape()) {
        ic = rtMegaPropProbe(fnObj->shape, keyIndex);
    }
    if (ic == nullptr || !ic->describesForFunction(fnObj->shape)) return false;
    ObjectHeader* holder = fnObj;
    if (ic->isStatics()) {
        if (!rtStaticsArmServes(fnVal)) return false;
        bool crossedDictionary = false;
        holder = rtStaticsAncestor(fnObj, ic->staticsDepth(), &crossedDictionary);
        if (holder == nullptr) return false;
    }
    if (!ic->isAccessor()) {
        out = holder->getSlot(ic->cached_slot).rawBits();
        return true;
    }
    // A getter sees the function, not the base class it is kept in.
    Rooted<Value> self{fnVal};
    Rooted<Value> getter{holder->getSlot(ic->cached_slot)};
    out = callGetter(getter.get(), self).rawBits();
    return true;
}

namespace {

// Every link `depth` up from a function's statics is a base constructor (or a
// plain object a program put on the chain) of a non-dictionary shape marked as
// a prototype, so an add to any of them bumps the epoch an inherited entry is
// filled at (InlineCache::describesForFunction).
bool statsChainMarked(ObjectHeader* fnObj, uint32_t depth) {
    ObjectHeader* cur = fnObj;
    for (uint32_t i = 0; i < depth; ++i) {
        if (!cur->shape) return false;
        const Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return false;
        const uint16_t kind = proto.asObject<HeapObjectHeader>()->flags;
        if (kind != HeapKind::Function && kind != HeapKind::Plain) return false;
        cur = proto.asObject<ObjectHeader>();
        if (!cur->shape || cur->shape->isDictionary() || !cur->shape->used_as_prototype) return false;
    }
    return true;
}

// Teach this site that the answer for a FUNCTION receiver is one of its
// statics, so the next read hits instead of walking this whole file again.
//
// An OWN static is the receiver's own slot at the receiver's own shape, the
// entry a plain object's own property gets: generated code's way-0 guard
// compares the shape word every object value has and loads the slot, with no
// kind test (heap.h carriesShape). An own ACCESSOR is filled too, as the
// accessor entry a plain object's own accessor gets.
//
// A static INHERITED through `extends` (`depth` links up the statics chain,
// every link a base class) is filled with BRONZE_ABI_IC_DEPTH_STATICS_FLAG,
// which only the function arms take (InlineCache::describesForFunction): a
// plain object's chain entry at the same shape would name %Object.prototype%,
// which is not on a function's path to its members, and the flag keeps the
// two apart. Only for a function the arms serve (staticsArmServes).
void installStaticsCacheEntry(Value fnVal, InlineCacheSite* site, ObjectHeader* propsObj,
                              const PropertyInfo& found, uint32_t depth) {
    if (site == nullptr) return;
    if (!rtStaticsCacheable(fnVal, propsObj)) return;
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

// The keys a function's ladder answers between its own statics and the ones
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
    // in its own slots (rtPrepareFunctionOwnWrite), and the table stops
    // answering them. A name the program put there is then found by the own
    // statics block below BEFORE the table's unimplemented-member diagnosis
    // can refuse it — a polyfill for a member bronze has not built is the
    // program's to add. Every entry in `kCtors` is a native function, so
    // source functions skip both the pre-probe and the table lookup.
    if (recv.get().asObject<FunctionHeader>()->isNativeCode()) {
        bool boxFirst = false;
        if (recv.get().asObject<FunctionHeader>()->statics_materialized) {
            const Shape* own_shape = recv.get().asObject<FunctionHeader>()->shape;
            PropertyInfo own;
            boxFirst = own_shape && own_shape->lookupProperty(PropertyKey::forString(keyHeader), own);
        }
        if (Value ctorMember;
            !boxFirst && rtGlobalConstructorMember(recv.get(), keyText(), ctorMember)) {
            return ctorMember.rawBits();
        }
    }
    // `prototype` lives in its own header slot; every other own property is
    // one of the function's statics, found through the statics chain
    // `extends` rooted at the base class. Reading `prototype` first is what
    // keeps `call`, `bind` and `name` answered or diagnosed rather than read
    // as undefined.
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
    // The function's OWN statics. The function is read through its root,
    // `recv`, here and again for the statics `extends` linked in below, with
    // the entire miss ladder in between: one probe there that builds an
    // intrinsic on first use is enough to relocate it.
    {
        ObjectHeader* propsObj = recv.get().asObject<FunctionHeader>()->object();
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
    {
        // The chain's end BEFORE the pointer that walks toward it:
        // `rtObjectPrototype` builds %Object.prototype% on first use, so a
        // `propsObj` read out first would be the address the function had
        // before that allocation. Same statement, opposite order, and only one
        // of the two orders survives a collection here. (A chain a program
        // ran into %Object.prototype% with `setPrototypeOf` stops there: its
        // members are the ladder's last step.)
        const uint64_t objProtoBits = rtObjectPrototype().rawBits();
        ObjectHeader* propsObj = recv.get().asObject<FunctionHeader>()->object();
        for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
            ObjectHeader* ancestor = rtStaticsAncestor(propsObj, depth);
            if (!ancestor || Value::fromObject(ancestor).rawBits() == objProtoBits) break;
            PropertyInfo inherited;
            if (ancestor->shape &&
                ancestor->shape->lookupProperty(PropertyKey::forString(keyHeader), inherited)) {
                // The ladder's steps between the own statics and here
                // answered nothing for this key (they returned above), and
                // for any other function the arms serve they answer by key
                // alone, so the entry is the answer for every function at
                // this shape while the epoch holds.
                if (!keyAnsweredBeforeInheritedStatics(keyHeader) &&
                    statsChainMarked(propsObj, depth)) {
                    installStaticsCacheEntry(recv.get(), site, propsObj, inherited, depth);
                }
                // Read off the holder, whose own property it is, with the
                // receiver handed down so a getter sees the class.
                Rooted<Value> key(Value::fromString(keyHeader));
                return ancestor->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
            }
        }
    }
    // %Function.prototype%'s own members (20.2.3): `call`, `apply`, `bind`,
    // `toString` and `constructor`, held in ITS statics, which every function
    // receiver's chain reaches whether or not the receiver has statics of its
    // own — a closure that was never assigned a static has none, and must
    // still read `call`. The receiver is handed to the read so an accessor
    // installed there would see the function. `rtFunctionPrototypeObject`
    // builds the intrinsics on first use, which is why it is taken through a
    // root afterwards.
    // A realm's addition to a frozen builtin function itself (`Object.x = f`)
    // is nearer than %Function.prototype% (intrinsic_additions.h).
    if (Value added; rtIntrinsicAdditionAt(recv.get().asObject<FunctionHeader>()->object(),
                                           PropertyKey::forString(keyHeader), added)) {
        return added.rawBits();
    }
    {
        Rooted<Value> fpBox{rtFunctionPrototypeObject()};
        ObjectHeader* fpObj = fpBox.get().asObject<FunctionHeader>()->object();
        PropertyInfo inherited;
        if (fpObj->shape->lookupProperty(PropertyKey::forString(keyHeader), inherited)) {
            Rooted<Value> key(Value::fromString(keyHeader));
            return fpObj->getProp(rtHeap(), key, /*ic=*/nullptr, recv.slot_ptr()).rawBits();
        }
        // And its addition to %Function.prototype% (`Function.prototype.method
        // = f`).
        if (Value added; rtIntrinsicAdditionAt(fpObj, PropertyKey::forString(keyHeader), added)) {
            return added.rawBits();
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

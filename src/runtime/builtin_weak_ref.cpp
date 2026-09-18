// The JS surface of `WeakRef` (ECMA-262 26.1) and `FinalizationRegistry`
// (26.2): the two constructors a bare name resolves to, %WeakRef.prototype%
// and %FinalizationRegistry.prototype% with the three methods they define,
// and the brands. The weakness itself — the tables, the post-collection
// sweep, the kept-objects list — is weak_ref.{h,cpp}, and this file has no
// opinion about it.
//
// The arrangement is builtin_weak_map.cpp's, deliberately: an instance is an
// ordinary object with internal slots, its prototype a real object chained to
// `Object.prototype`, and every method opens with a brand check so a detached
// `const d = wr.deref; d()` names the receiver it did not get rather than
// reading some other object's slots as a weak target.
//
// `FinalizationRegistry.prototype.cleanupSome` is NOT here: it is a stage-2
// proposal, not ECMA-262, and what it asks for is the one thing this design
// deliberately does not offer — draining the cleanup queue SYNCHRONOUSLY, from
// inside whatever expression called it, rather than from a job. Reading it
// answers `undefined`, exactly as node does.

#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/map.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/symbol.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

namespace {

// As builtin_map.cpp's record: one per kind, every object a permanent root.
struct WeakRefIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Value brand = Value::fromUndefined();
    Shape* instanceShape = nullptr;
};

thread_local WeakRefIntrinsics g_weakRef;
thread_local WeakRefIntrinsics g_registry;

void ensureWeakRefIntrinsics();

bool isCallableValue(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// The brand test, on the slot count the kind was created with: a WeakRef and
// a registry are both three slots, and it is the symbol that tells them
// apart. `MapHeader::hasBrand` asks the same three questions for its seven.
bool hasBrand(Value v, Value brand, uint32_t slotCount) {
    if (!brand.isSymbol() || !v.isObject()) return false;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Plain) return false;
    const auto* obj = reinterpret_cast<const ObjectHeader*>(hdr);
    if (obj->internalSlotCount() != slotCount) return false;
    return obj->internalSlot(0).rawBits() == brand.rawBits();
}

bool requireKind(Value self, const WeakRefIntrinsics& kind, uint32_t slotCount,
                 const char* method) {
    if (hasBrand(self, kind.brand, slotCount)) return true;
    rtThrowTypeError("Method " + std::string(method) + " called on an incompatible receiver");
    return false;
}

// ---- WeakRef ----------------------------------------------------------------

// 26.1.1.1. The receiver is the object `new` allocated from NewTarget
// (builtin_map.cpp's `buildCollection` says why filling it in place is what
// makes a subclass work). Step 2 is the CanBeHeldWeakly test and it is a
// TypeError, not a quiet nothing: a WeakRef over a value that can never become
// unreachable is a strong reference the program would believe was weak.
uint64_t weakRefConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    if (!rtIsNativeConstructReceiver(receiver.get()) ||
        !hasBrand(receiver.get(), g_weakRef.brand, WeakRefSlot::kCount)) {
        return rtThrowTypeError("Constructor WeakRef requires 'new'").rawBits();
    }
    if (!rtCanBeHeldWeakly(args[0])) {
        return rtThrowTypeError("Invalid value used in WeakRef (an object or an unregistered "
                                "symbol can be held weakly; nothing else can become unreachable)")
            .rawBits();
    }
    Rooted<Value> target{args[0]};
    rtWeakRefInit(receiver, target);
    return receiver.get().rawBits();
}

// 26.1.3.2.
uint64_t weakRefDeref(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireKind(self.get(), g_weakRef, WeakRefSlot::kCount, "WeakRef.prototype.deref")) {
        return Value::fromUndefined().rawBits();
    }
    return rtWeakRefDeref(self.get()).rawBits();
}

// ---- FinalizationRegistry ---------------------------------------------------

// 26.2.1.1. Step 2: a non-callable cleanup callback is a TypeError at
// CONSTRUCTION, which is the only place the mistake is still cheap to name — a
// cleanup job has no caller to report to.
uint64_t finalizationRegistryConstructor(uint64_t, uint64_t thisBits, uint32_t argc,
                                         const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    if (!rtIsNativeConstructReceiver(receiver.get()) ||
        !hasBrand(receiver.get(), g_registry.brand, RegistrySlot::kCount)) {
        return rtThrowTypeError("Constructor FinalizationRegistry requires 'new'").rawBits();
    }
    if (!isCallableValue(args[0])) {
        return rtThrowTypeError("FinalizationRegistry requires a callable cleanup callback")
            .rawBits();
    }
    Rooted<Value> callback{args[0]};
    rtFinalizationRegistryInit(receiver, callback);
    return receiver.get().rawBits();
}

// 26.2.3.1 register(target, heldValue, unregisterToken).
uint64_t finalizationRegister(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireKind(self.get(), g_registry, RegistrySlot::kCount,
                     "FinalizationRegistry.prototype.register")) {
        return Value::fromUndefined().rawBits();
    }
    if (!rtCanBeHeldWeakly(args[0])) {
        return rtThrowTypeError("Invalid value used as the target of a FinalizationRegistry "
                                "registration")
            .rawBits();
    }
    // Step 4: `target` and `heldValue` being the SAME value would make the
    // registry hold its own target alive through the held value, so the cell
    // could never fire. The specification refuses it rather than accepting a
    // registration that cannot work.
    if (args[0].rawBits() == args[1].rawBits()) {
        return rtThrowTypeError("A FinalizationRegistry's target and held value must differ (a "
                                "held value is retained STRONGLY, so registering the target as "
                                "its own held value could never fire)")
            .rawBits();
    }
    // Step 5: a token that is neither undefined nor weakly-holdable is refused;
    // `undefined` means "no token", and a registration with none simply cannot
    // be unregistered.
    if (!args[2].isUndefined() && !rtCanBeHeldWeakly(args[2])) {
        return rtThrowTypeError("Invalid value used as a FinalizationRegistry unregister token")
            .rawBits();
    }
    Rooted<Value> target{args[0]};
    Rooted<Value> held{args[1]};
    Rooted<Value> token{args[2]};
    rtFinalizationRegister(self, target, held, token);
    return Value::fromUndefined().rawBits();
}

// 26.2.3.2 unregister(unregisterToken).
uint64_t finalizationUnregister(uint64_t, uint64_t thisBits, uint32_t argc,
                                const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireKind(self.get(), g_registry, RegistrySlot::kCount,
                     "FinalizationRegistry.prototype.unregister")) {
        return Value::fromUndefined().rawBits();
    }
    if (!rtCanBeHeldWeakly(args[0])) {
        return rtThrowTypeError("Invalid value used as a FinalizationRegistry unregister token")
            .rawBits();
    }
    Rooted<Value> token{args[0]};
    return Value::fromBool(rtFinalizationUnregister(self, token)).rawBits();
}

const NativeMethod kWeakRefMethods[] = {
    {"deref", weakRefDeref, 0, 0},
};

const NativeMethod kRegistryMethods[] = {
    {"register", finalizationRegister, 2, 2},
    {"unregister", finalizationUnregister, 1, 1},
};

// ---- assembling the intrinsics ----------------------------------------------

void buildKind(WeakRefIntrinsics& out, bool isRegistry) {
    Rooted<Value> parent{rtObjectPrototype()};
    Shape* protoShape = rtNewRootShape(parent.get());
    protoShape->used_as_prototype = true;
    ObjectHeader* protoObj = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};
    out.proto = proto.get();
    rtHeap().add_permanent_root(&out.proto);

    // 26.1.3.3 / 26.2.3.3 — the only reason `Object.prototype.toString.call(
    // wr)` reads "[object WeakRef]": 20.1.3.6's builtin-tag list has no entry
    // for either, so step 14 would have said "Object" without it.
    rtDefineToStringTag(proto, isRegistry ? "FinalizationRegistry" : "WeakRef");

    // 26.1.2 / 26.2.2: both constructors have length 1.
    Rooted<Value> ctor{rtNativeFunction(
        isRegistry ? finalizationRegistryConstructor : weakRefConstructor, 1,
        isRegistry ? "FinalizationRegistry" : "WeakRef", 1)};
    out.ctor = ctor.get();
    rtHeap().add_permanent_root(&out.ctor);
    {
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    if (isRegistry) {
        rtDefineMethods(proto, kRegistryMethods, std::size(kRegistryMethods));
    } else {
        rtDefineMethods(proto, kWeakRefMethods, std::size(kWeakRefMethods));
    }

    out.brand = rtMakeSymbol(Value::fromUndefined());

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    fn->instance_shape = rtNewRootShape(proto.get());
    out.instanceShape = fn->instance_shape;
}

void ensureWeakRefIntrinsics() {
    if (g_weakRef.proto.isObject()) return;
    buildKind(g_weakRef, /*isRegistry=*/false);
    buildKind(g_registry, /*isRegistry=*/true);
}

// The object and its brand, with the other slots in their empty state.
Value allocate(Shape* shape, bool isRegistry) {
    const WeakRefIntrinsics& kind = isRegistry ? g_registry : g_weakRef;
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(
        rtHeap(), rtArena(), shape, isRegistry ? RegistrySlot::kCount : WeakRefSlot::kCount);
    obj->header.flags = HeapKind::Plain;
    obj->setInternalSlot(0, kind.brand);
    const Value val = Value::fromObject(obj);
    rtWeakSlotsReset(val, isRegistry);
    return val;
}

}  // namespace

Value rtWeakRefConstructor(const std::string& name) {
    if (name != "WeakRef" && name != "FinalizationRegistry") return Value::fromUndefined();
    ensureWeakRefIntrinsics();
    return name == "WeakRef" ? g_weakRef.ctor : g_registry.ctor;
}

// By CODE POINTER and never by interning a constructor and comparing bits:
// identifying an intrinsic must not build one, because this is called from
// paths where an unexpected allocation retires a pointer mid-lookup.
const char* rtWeakRefConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    if (code == weakRefConstructor) return "WeakRef";
    if (code == finalizationRegistryConstructor) return "FinalizationRegistry";
    return nullptr;
}

bool rtIsWeakRefObject(Value v) { return hasBrand(v, g_weakRef.brand, WeakRefSlot::kCount); }

bool rtIsFinalizationRegistryObject(Value v) {
    return hasBrand(v, g_registry.brand, RegistrySlot::kCount);
}

Value rtNewWeakRefWithShape(Shape* shape, bool isRegistry) {
    ensureWeakRefIntrinsics();
    return allocate(shape, isRegistry);
}

Value rtNewWeakRef(Rooted<Value>& target) {
    ensureWeakRefIntrinsics();
    Rooted<Value> self{allocate(g_weakRef.instanceShape, /*isRegistry=*/false)};
    rtWeakRefInit(self, target);
    return self.get();
}

Value rtNewFinalizationRegistry(Rooted<Value>& callback) {
    ensureWeakRefIntrinsics();
    Rooted<Value> self{allocate(g_registry.instanceShape, /*isRegistry=*/true)};
    rtFinalizationRegistryInit(self, callback);
    return self.get();
}

}  // namespace bronze::runtime

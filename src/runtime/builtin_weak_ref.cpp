// The JS surface of `WeakRef` (ECMA-262 26.1) and `FinalizationRegistry`
// (26.2): the two constructors a bare name resolves to, the three methods they
// define, and the member tables the property path answers from. The weakness
// itself — the tables, the post-collection sweep, the kept-objects list — is
// weak_ref.{h,cpp}, and this file has no opinion about it.
//
// The arrangement is builtin_weak_map.cpp's, deliberately: methods are ordinary
// function objects handed out by the property path, there is no
// `WeakRef.prototype` OBJECT (so `WeakRef.prototype` is a named refusal and
// `wr instanceof WeakRef` is false), and every method opens with a brand check
// so a detached `const d = wr.deref; d()` names the receiver it did not get
// rather than reading some other object's bytes as a weak slot.
//
// `FinalizationRegistry.prototype.cleanupSome` is refused BY NAME. It is not
// standard ECMA-262 — it is a stage-2 proposal — and what it asks for is the
// one thing this design deliberately does not offer: draining the cleanup queue
// SYNCHRONOUSLY, from inside whatever expression called it, rather than from a
// job. Answering `undefined` for it would let a program believe it had forced
// a cleanup that never happened.

#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

namespace {

bool isCallableValue(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

bool requireKind(Value self, uint16_t flags, const char* method) {
    if (self.isObject() && self.asObject<HeapObjectHeader>()->flags == flags) return true;
    rtThrowTypeError("Method " + std::string(method) + " called on an incompatible receiver");
    return false;
}

// ---- WeakRef ----------------------------------------------------------------

// 26.1.1.1. Step 2 is the CanBeHeldWeakly test and it is a TypeError, not a
// quiet nothing: a WeakRef over a value that can never become unreachable is a
// strong reference the program would believe was weak.
uint64_t weakRefConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!rtCanBeHeldWeakly(args[0])) {
        return rtThrowTypeError("Invalid value used in WeakRef (an object or an unregistered "
                                "symbol can be held weakly; nothing else can become unreachable)")
            .rawBits();
    }
    Rooted<Value> target{args[0]};
    return rtMakeWeakRef(target).rawBits();
}

// 26.1.3.2.
uint64_t weakRefDeref(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireKind(self.get(), WeakRefHeader::kFlags, "WeakRef.prototype.deref")) {
        return Value::fromUndefined().rawBits();
    }
    return rtWeakRefDeref(self.get()).rawBits();
}

// ---- FinalizationRegistry ---------------------------------------------------

// 26.2.1.1. Step 2: a non-callable cleanup callback is a TypeError at
// CONSTRUCTION, which is the only place the mistake is still cheap to name — a
// cleanup job has no caller to report to.
uint64_t finalizationRegistryConstructor(uint64_t, uint64_t, uint32_t argc,
                                         const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isCallableValue(args[0])) {
        return rtThrowTypeError("FinalizationRegistry requires a callable cleanup callback")
            .rawBits();
    }
    Rooted<Value> callback{args[0]};
    return rtMakeFinalizationRegistry(callback).rawBits();
}

// 26.2.3.1 register(target, heldValue, unregisterToken).
uint64_t finalizationRegister(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireKind(self.get(), FinalizationRegistryHeader::kFlags,
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
    if (!requireKind(self.get(), FinalizationRegistryHeader::kFlags,
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

struct Method {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const Method kWeakRefMethods[] = {
    {"deref", weakRefDeref, 0},
};

const Method kRegistryMethods[] = {
    {"register", finalizationRegister, 2},
    {"unregister", finalizationUnregister, 1},
};

// `prototype` and `constructor` for the reason every other intrinsic in this
// runtime lists them: bronze builds no prototype OBJECT here, and `undefined`
// would let a program install a method nothing would ever find.
const char* const kWeakRefUnimplemented[] = {
    "constructor", "prototype",
};

const char* const kRegistryUnimplemented[] = {
    "constructor", "prototype",
    // Not ECMA-262 (a stage-2 proposal) and listed anyway, because a program
    // that calls it is asking for a SYNCHRONOUS drain of the cleanup queue and
    // must be told bronze runs cleanup from the job queue only.
    "cleanupSome",
};

}  // namespace

Value rtWeakRefConstructor(const std::string& name) {
    if (name == "WeakRef") return rtNativeFunction(weakRefConstructor, 1);
    if (name == "FinalizationRegistry") {
        return rtNativeFunction(finalizationRegistryConstructor, 1);
    }
    return Value::fromUndefined();
}

// By CODE POINTER and never by interning a constructor and comparing bits:
// identifying an intrinsic must not build one, because this is called from the
// property path's miss ladder where an unexpected allocation retires the
// property box mid-lookup.
const char* rtWeakRefConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    if (code == weakRefConstructor) return "WeakRef";
    if (code == finalizationRegistryConstructor) return "FinalizationRegistry";
    return nullptr;
}

Value rtWeakRefMember(Value self, const std::string& key) {
    const bool isRegistry =
        self.asObject<HeapObjectHeader>()->flags == FinalizationRegistryHeader::kFlags;
    if (isRegistry) {
        for (const Method& m : kRegistryMethods) {
            if (key == m.name) return rtNativeFunction(m.code, m.arity);
        }
    } else {
        for (const Method& m : kWeakRefMethods) {
            if (key == m.name) return rtNativeFunction(m.code, m.arity);
        }
    }
    rtCheckWeakRefMember(isRegistry, key);
    return Value::fromUndefined();
}

// `in`'s half, off the same tables the read path answers from — the one-list
// rule builtin_weak_map.cpp states.
bool rtWeakRefHasMember(Value self, const std::string& key) {
    const bool isRegistry =
        self.asObject<HeapObjectHeader>()->flags == FinalizationRegistryHeader::kFlags;
    if (isRegistry) {
        for (const Method& m : kRegistryMethods) {
            if (key == m.name) return true;
        }
    } else {
        for (const Method& m : kWeakRefMethods) {
            if (key == m.name) return true;
        }
    }
    rtCheckWeakRefMember(isRegistry, key);
    return false;
}

void rtCheckWeakRefMember(bool isRegistry, const std::string& key) {
    if (isRegistry) {
        rtCheckUnimplementedMember("FinalizationRegistry.prototype", kRegistryUnimplemented,
                                   std::size(kRegistryUnimplemented), key);
        return;
    }
    rtCheckUnimplementedMember("WeakRef.prototype", kWeakRefUnimplemented,
                               std::size(kWeakRefUnimplemented), key);
}

}  // namespace bronze::runtime

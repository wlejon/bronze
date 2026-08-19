// %GeneratorFunction%, %AsyncFunction% and %AsyncGeneratorFunction% — the three
// intrinsic constructors ECMA-262 27.3, 27.7 and 27.4 give the function forms
// that are not ordinary functions, and the prototype object each one carries.
//
// None of the three is a global. The only way a program names one is through a
// function of that form — `Object.getPrototypeOf(function* () {})` is
// %GeneratorFunction.prototype%, and its `constructor` is %GeneratorFunction%
// — which is why they are built here rather than in the builtin ladder:
// nothing resolves them by name, and the RECEIVER is what selects between them.
//
// They exist because answering `Function` for all three is a WRONG answer and
// not a missing one. `g.constructor === Function` reads true in no engine, and
// the idiom that asks — deciding whether a value is a generator function by
// comparing its constructor, which is how code does it without reaching for
// `@@toStringTag` — got the same answer for every function in the program.
//
// CALLING one is dynamic code compilation (27.3.1.1 is CreateDynamicFunction
// with a generator body), so all three refuse exactly as `Function` does. The
// same sentence, because it is the same reason.

#include <cstdint>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/host_globals.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The kinds, indexed by `(generator ? 1 : 0) | (async ? 2 : 0)`. Slot 0 — an
// ordinary function — stays permanently empty: its constructor is `Function`
// and its prototype %Function.prototype%, both of which already have answers
// elsewhere, and a fourth entry here would be a second one.
enum KindSlot : uint32_t {
    kOrdinary = 0,
    kGenerator = 1,
    kAsync = 2,
    kAsyncGenerator = 3,
    kKindCount = 4,
};

constexpr const char* kindName(uint32_t slot) {
    switch (slot) {
        case kGenerator: return "GeneratorFunction";
        case kAsync: return "AsyncFunction";
        case kAsyncGenerator: return "AsyncGeneratorFunction";
        default: return nullptr;
    }
}

// THREE instantiations, so three distinct code pointers, so three distinct
// interned function objects — `rtNativeFunction` interns on the code pointer,
// and one shared body made all three constructors the SAME object: reading
// `a.constructor` handed back the generator's, `g.constructor` then answered
// with the async one, and both `===` comparisons still looked consistent
// because there was only ever one function to compare.
//
// The message names the kind, and that is what keeps the three bodies
// genuinely distinct: identical bodies are folded back into one by the
// linker's identical-code elimination, which put the three back at one address
// after the template alone had separated them.
//
// A host that installed an answer (host_globals.h, rtSetDynamicFunctionHost)
// gets asked here too, and gets told WHICH of the four it is: %AsyncFunction%
// and %GeneratorFunction% build genuinely different callables, and a host that
// answered all four the same way would be handing back a plain function where
// the program is about to `await` or `next()` it. `Slot` is the same index the
// enum uses, which is why the enum was given those values.
template <uint32_t Slot>
uint64_t dynamicFunctionRefusal(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (const DynamicFunctionHost& host = rtDynamicFunctionHost()) {
        RootedArgs args(argc, argv);
        return host(static_cast<DynamicFunctionKind>(Slot),
                    std::span<const Value>(args.data(), args.count()))
            .rawBits();
    }
    return rtThrowTypeError(
               std::string(kindName(Slot)) +
               ": dynamic code compilation from strings is out of scope for an AOT compiler")
        .rawBits();
}

struct KindIntrinsic {
    Value ctor{Value::fromUndefined()};
    Value proto{Value::fromUndefined()};
};

thread_local KindIntrinsic g_kinds[kKindCount];

// The kind prototype's own `prototype` property: what every object a function
// of this kind produces inherits from. %GeneratorFunction.prototype.prototype%
// IS %GeneratorPrototype% (27.3.3.3) — the object a generator's `next` lives
// on — so the two are the same object and not a copy. An ASYNC function has no
// such property at all (27.7.3 lists none), because its result is a promise
// and not an object of a kind of its own.
Value instancePrototypeFor(uint32_t slot) {
    switch (slot) {
        case kGenerator: return rtIteratorPrototype(IteratorProto::Generator);
        case kAsyncGenerator: return rtIteratorPrototype(IteratorProto::AsyncGenerator);
        default: return Value::fromUndefined();
    }
}

void ensureKind(uint32_t slot) {
    if (g_kinds[slot].ctor.isObject()) return;
    const char* name = kindName(slot);
    if (!name) return;

    // The instance prototype FIRST: reading it builds %GeneratorPrototype% on
    // first use, and that allocation would move anything read before it.
    Rooted<Value> instanceProto{instancePrototypeFor(slot)};

    // 27.3.3: an ORDINARY object, not a callable one — which is where it
    // differs from %Function.prototype%, and why `typeof` of it is "object".
    // Its [[Prototype]] is %Function.prototype%, and `protoAncestor` ends a
    // chain at a link that is not a plain object, so the members it inherits
    // are answered by the named refusal below rather than read as `undefined`.
    Rooted<Value> proto{Value::fromObject(ObjectHeader::create(
        rtHeap(), rtArena(), rtRootShapeForPrototype(rtFunctionPrototypeObject())))};
    proto.get().asObject<HeapObjectHeader>()->flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;

    Rooted<Value> ctor{rtNativeFunction(slot == kGenerator ? dynamicFunctionRefusal<kGenerator>
                                        : slot == kAsync    ? dynamicFunctionRefusal<kAsync>
                                                     : dynamicFunctionRefusal<kAsyncGenerator>,
                                        1)};
    ctor.get().asObject<FunctionHeader>()->name =
        StringHeader::internToArena(rtArena(), StringHeader::createFromUTF8(rtHeap(), name));
    ctor.get().asObject<FunctionHeader>()->length = 1;
    ctor.get().asObject<FunctionHeader>()->prototype = proto.get();
    // 27.3.2.2: the `prototype` of each of these is non-writable and
    // non-configurable, unlike an ordinary function's.
    ctor.get().asObject<FunctionHeader>()->prototype_readonly = true;

    g_kinds[slot].ctor = ctor.get();
    g_kinds[slot].proto = proto.get();
    rtHeap().add_permanent_root(&g_kinds[slot].ctor);
    rtHeap().add_permanent_root(&g_kinds[slot].proto);

    Rooted<Value> ctorKey{rtMakeString("constructor")};
    proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), ctorKey, ctor,
                                                  /*ic=*/nullptr, /*enumerable=*/false,
                                                  /*defineOwn=*/true);
    if (instanceProto.get().isObject()) {
        Rooted<Value> protoKey{rtMakeString("prototype")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), protoKey, instanceProto,
                                                      /*ic=*/nullptr, /*enumerable=*/false,
                                                      /*defineOwn=*/true);
    }
    rtDefineToStringTag(proto, name);
    // Re-read through the local roots: every write above can collect, and the
    // permanent roots were installed before those allocations happened.
    g_kinds[slot].ctor = ctor.get();
    g_kinds[slot].proto = proto.get();
}

// Which slot a value belongs to; `kOrdinary` for anything that is not a
// function of one of the three forms.
uint32_t slotForFunction(Value fnVal) {
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return kOrdinary;
    }
    const FunctionHeader* fn = fnVal.asObject<FunctionHeader>();
    return (fn->isGeneratorFunction() ? 1u : 0u) | (fn->isAsyncFunction() ? 2u : 0u);
}

// What 20.2.3 puts on %Function.prototype% and 10.2 on every function object.
// A read of one of these off a kind's prototype object walks a chain that ends
// at a link `protoAncestor` will not cross, so it would answer `undefined` —
// a wrong answer about a property the language really does inherit, which is
// what this table turns into a diagnostic.
const char* const kFunctionProtoMembers[] = {
    "apply", "bind", "call", "toString", "length", "name", "caller", "arguments",
};

}  // namespace

Value rtFunctionKindConstructor(Value fnVal) {
    const uint32_t slot = slotForFunction(fnVal);
    if (slot == kOrdinary) return Value::fromUndefined();
    ensureKind(slot);
    return g_kinds[slot].ctor;
}

Value rtFunctionKindPrototype(Value fnVal) {
    const uint32_t slot = slotForFunction(fnVal);
    if (slot == kOrdinary) return Value::fromUndefined();
    ensureKind(slot);
    return g_kinds[slot].proto;
}

void rtFunctionKindCheckMissingMember(Value obj, const std::string& key) {
    for (uint32_t slot = kGenerator; slot < kKindCount; ++slot) {
        if (!g_kinds[slot].proto.isObject()) continue;
        if (obj.rawBits() != g_kinds[slot].proto.rawBits()) continue;
        const std::string receiver = std::string(kindName(slot)) + ".prototype";
        rtCheckUnimplementedMember(receiver.c_str(), kFunctionProtoMembers,
                                   std::size(kFunctionProtoMembers), key);
        return;
    }
}

}  // namespace bronze::runtime

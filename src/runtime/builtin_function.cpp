// `Function` and `Function.prototype` — the Function global constructor and
// its prototype object (ECMA-262 20.2), holder of `call`, `apply`, `bind`,
// `toString` and `constructor`.
//
// `Function.prototype` is itself a callable function object (20.2.3) that
// returns undefined when called. Constructor-from-string (`new Function(...)`
// or `Function(...)`) is out of scope for an AOT compiler and is diagnosed by
// name with a TypeError.
//
// `bind` is a row here and a body elsewhere: it has to MAKE a function, and
// the making — the trampoline, the cell that holds [[BoundTargetFunction]],
// [[BoundThis]] and [[BoundArguments]], and the construct-path unwrapping —
// is builtin_function_bind.cpp's whole subject.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 20.2.1 The Function Constructor.
// Constructor-from-string (`new Function(...)` or `Function(...)`) is out of scope
// for an AOT compiler — refused by name.
uint64_t functionConstructorBody(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return rtThrowTypeError(
               "Function: dynamic code compilation from strings is out of scope for an AOT compiler")
        .rawBits();
}

// 20.2.3 The Function Prototype Object is itself a built-in function object.
// When called, it accepts any arguments and returns undefined.
uint64_t functionPrototypeBody(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromUndefined().rawBits();
}

static Value g_functionPrototype = Value::fromUndefined();
static Value g_functionConstructor = Value::fromUndefined();

void ensureFunctionIntrinsics() {
    if (g_functionPrototype.isObject()) return;

    // Intern immortal arena strings FIRST, before allocating heap function objects.
    StringHeader* emptyName = StringHeader::internToArena(
        rtArena(), StringHeader::createFromUTF8(rtHeap(), ""));
    StringHeader* fnName = StringHeader::internToArena(
        rtArena(), StringHeader::createFromUTF8(rtHeap(), "Function"));

    // 20.2.3: Function.prototype is a callable function object.
    Rooted<Value> proto{rtNativeFunction(functionPrototypeBody, 0)};
    proto.get().asObject<FunctionHeader>()->name = emptyName;
    proto.get().asObject<FunctionHeader>()->length = 0;

    // 20.2.1: Function constructor object.
    Rooted<Value> ctor{rtNativeFunction(functionConstructorBody, 1)};
    ctor.get().asObject<FunctionHeader>()->name = fnName;
    ctor.get().asObject<FunctionHeader>()->length = 1;
    ctor.get().asObject<FunctionHeader>()->prototype = proto.get();
    ctor.get().asObject<FunctionHeader>()->instance_shape =
        rtRootShapeForPrototype(proto.get());

    g_functionPrototype = proto.get();
    g_functionConstructor = ctor.get();
    rtHeap().add_permanent_root(&g_functionPrototype);
    rtHeap().add_permanent_root(&g_functionConstructor);

    rtEnsureFunctionProperties(proto);
    Rooted<Value> hasInstKey{Value::fromSymbol(rtSymbolHasInstance())};
    Rooted<Value> hasInstFn{rtNativeFunction(rtFunctionHasInstanceBuiltin, 1)};
    proto.get().asObject<FunctionHeader>()->properties.asObject<ObjectHeader>()->setProp(
        rtHeap(), rtArena(), hasInstKey, hasInstFn, nullptr, /*enumerable=*/false, /*defineOwn=*/true);
}

bool requireFunctionReceiver(Value self, const char* method) {
    if (self.isObject() && self.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        return true;
    }
    rtThrowTypeError(std::string("Function.prototype.") + method +
                     " called on a value that is not a function");
    return false;
}

// 20.2.3.3 Function.prototype.call(thisArg, ...args).
//
// The argument block is a RootedBlock rather than `argv + 1`. `argv` is the raw
// block this helper was entered with, and `bronze_dynamic_call` may reach a
// callee that allocates before it reads it — so the values are re-rooted here
// rather than assumed to still be where the caller left them.
uint64_t functionCall(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> fn{Value(thisBits)};
    if (!requireFunctionReceiver(fn.get(), "call")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[0]};

    const uint32_t forwarded = args.count() > 1 ? args.count() - 1 : 0;
    RootedBlock block(forwarded);
    for (uint32_t i = 0; i < forwarded; ++i) block.set(i, args[i + 1]);
    return bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), forwarded,
                               block.data());
}

// 20.2.3.1 Function.prototype.apply(thisArg, argArray).
//
// `null` and `undefined` for the array mean "no arguments" (step 3), which is
// what separates `f.apply(o)` from `f.apply(o, [])` in spelling only. Anything
// else goes through 7.3.19 CreateListFromArrayLike, which accepts ANY object
// with a `length` — a real array, an `arguments` object, a NodeList, a
// hand-written `{length: 2, 0: "a", 1: "b"}`. A PRIMITIVE is the one refusal
// step 1 names, and it is a catchable TypeError rather than a fatal, because
// the specification says exactly what that call means.
//
// `f.apply(null, arrayLike)` is how a program spells "call with these
// arguments" when it does not have an array in hand, and it is common enough in
// library code that refusing it stopped real programs. The refusal it replaces
// said bronze had no array-like protocol; it has one now (rt_builtins.h), and
// `Array.from` and the typed-array constructors read the same one.
uint64_t functionApply(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> fn{Value(thisBits)};
    if (!requireFunctionReceiver(fn.get(), "apply")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[0]};
    Rooted<Value> list{args[1]};

    if (list.get().isNull() || list.get().isUndefined()) {
        RootedBlock empty(0);
        return bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), 0, empty.data());
    }
    if (!list.get().isObject()) {
        return rtThrowTypeError(
                   "CreateListFromArrayLike called on a non-object: Function.prototype.apply "
                   "needs an array-like argument list")
            .rawBits();
    }
    // The length is read before the block is built, and each element through
    // the rooted source — both reads can run a getter, and the block's
    // construction allocates.
    const uint32_t count = rtArrayLikeLength(list);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (!rtCheckAppliedArgumentCount(count, "Function.prototype.apply")) {
        return Value::fromUndefined().rawBits();
    }
    RootedBlock block(count);
    for (uint32_t i = 0; i < count; ++i) {
        block.set(i, rtArrayLikeElement(list, i));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), count, block.data());
}

// 20.2.3.5 Function.prototype.toString().
uint64_t functionToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireFunctionReceiver(self, "toString")) return Value::fromUndefined().rawBits();
    auto* fn = self.asObject<FunctionHeader>();
    if (fn->name && fn->name->getLength() > 0) {
        std::string name = rtUtf8Chars(fn->name);
        return rtMakeString("function " + name + "() { [native code] }").rawBits();
    }
    return rtMakeString("function () { [native code] }").rawBits();
}

struct FunctionMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const FunctionMethod kFunctionMethods[] = {
    {"apply", functionApply, 2},
    {"bind", rtFunctionBindBuiltin, 1},
    {"call", functionCall, 1},
    {"toString", functionToString, 0},
};

}  // namespace

Value rtFunctionConstructorObject() {
    ensureFunctionIntrinsics();
    return g_functionConstructor;
}

Value rtFunctionPrototypeObject() {
    ensureFunctionIntrinsics();
    return g_functionPrototype;
}

bool rtIsFunctionConstructor(Value fn) {
    if (!fn.isObject()) return false;
    HeapObjectHeader* hdr = fn.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Function) return false;
    return reinterpret_cast<FunctionHeader*>(hdr)->code == functionConstructorBody;
}

bool rtIsFunctionPrototype(Value fn) {
    if (!fn.isObject()) return false;
    HeapObjectHeader* hdr = fn.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Function) return false;
    return reinterpret_cast<FunctionHeader*>(hdr)->code == functionPrototypeBody;
}

Value rtFunctionMethod(const std::string& key) {
    if (key == "constructor") return rtFunctionConstructorObject();
    for (const FunctionMethod& m : kFunctionMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

uint64_t rtFunctionHasInstanceBuiltin(uint64_t, uint64_t thisBits, uint32_t argc,
                                      const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    Value v = args[0];
    return Value::fromBool(rtOrdinaryHasInstance(self, v)).rawBits();
}

}  // namespace bronze::runtime

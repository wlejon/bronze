// `Function.prototype.call` and `.apply` — the two members that make a function
// value usable as a function of its receiver.
//
// They are answered by the property path, beside a function rather than found
// on a `Function.prototype` object a program can hold: a FunctionHeader is not
// an ObjectHeader, so it has no shape and no prototype chain for a walk to
// follow. That is the same arrangement `Array.prototype`'s methods are still
// in, and it is why `Object.getPrototypeOf(f)` remains a named error while
// `Object.getPrototypeOf({})` no longer is — an intrinsic bronze hands out
// but cannot hand over is not one a program may claim to hold.
//
// `bind` is deliberately absent and stays diagnosed by name in rt_members.cpp.
// It is not a third member of this file: it has to MAKE a function — an exotic
// object with [[BoundTargetFunction]], [[BoundThis]] and [[BoundArguments]],
// whose `length` is the target's less the bound count — and bronze has one
// function representation, which holds a code pointer and an environment and no
// place to put any of that.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/rt_internal.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

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
// else must be a real array: CreateListFromArrayLike accepts any array-like,
// and bronze refuses the ones it cannot walk by name rather than treating a
// non-array as empty — silently dropping every argument is the worst available
// answer.
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
    if (!list.get().isObject() ||
        list.get().asObject<HeapObjectHeader>()->flags != HeapKind::Array) {
        fatal("unsupported: Function.prototype.apply with an argument list that is not an array "
              "(bronze has no array-like protocol; pass a real array)");
    }
    // The length is read before the block is built, and each element is read
    // through the rooted array — the block's construction allocates.
    const uint32_t count = list.get().asObject<ArrayHeader>()->length;
    RootedBlock block(count);
    for (uint32_t i = 0; i < count; ++i) {
        block.set(i, list.get().asObject<ArrayHeader>()->getElem(i));
    }
    return bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), count, block.data());
}

struct FunctionMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const FunctionMethod kFunctionMethods[] = {
    {"apply", functionApply, 2},
    {"call", functionCall, 1},
};

}  // namespace

Value rtFunctionMethod(const std::string& key) {
    for (const FunctionMethod& m : kFunctionMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

}  // namespace bronze::runtime

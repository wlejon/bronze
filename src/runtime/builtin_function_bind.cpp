// `Function.prototype.bind` (ECMA-262 20.2.3.2) and the bound function it
// makes (10.4.1).
//
// bronze has one function representation — a code pointer and an environment —
// so a bound function is not a new kind: it is a FunctionHeader whose code is
// the one trampoline below and whose env_record is a heap cell holding
// [[BoundTargetFunction]], [[BoundThis]] and [[BoundArguments]]. That is the
// arrangement chunk-one's host functions ride (embed_function.cpp), with the
// cell an EnvHeader instead of a native handle: every slot is a Value, so the
// collector's generic payload scan keeps the target, the receiver and every
// bound argument alive and current for exactly as long as the bound function
// is — no root source, no finalizer.
//
// `f.bind(a).bind(b)` NESTS: the outer bound function's target is the inner
// one, and its [[Call]] runs the inner trampoline, which prepends the inner
// args and replaces the receiver again — b is ignored, because the inner
// binding already fixed `this` to a. That is 20.2.3.2 run twice, literally,
// and 10.4.1.1 makes the nested call's answer identical to a flattened one's.
// [[Construct]] is where 10.4.1.2 demands the flattening — the instance's
// prototype must be the ULTIMATE target's — and bronze_construct gets it by
// recursion: it unwraps one bound layer, prepends that layer's args, and
// constructs the target, which unwraps the next (rt_object.cpp).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/env.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The env cell's slots. An EnvHeader and not a plain object: no shape, no
// keys, three Values the payload scan forwards — and a program can never
// reach it, since nothing hands the env_record of a function out.
enum BoundSlot : uint32_t { Target, BoundThis, BoundArgs, kBoundSlots };

Value readBoundSlot(Rooted<Value>& env, uint32_t slot) {
    return env.get().asObject<EnvHeader>()->slotsData()[slot];
}

// One trampoline for every bound function; what tells two apart is the env
// cell. Built through FunctionHeader::create and never the singleton table —
// interning on this shared code pointer would collapse every bound function
// in the program into the first one, exactly as it would collapse closures.
uint64_t boundCallTrampoline(uint64_t envBits, uint64_t thisBits, uint32_t argc,
                             const uint64_t* argv) {
    // The builtin prologue: arguments into roots before anything can allocate.
    // `thisBits` is DISCARDED after this line, which is 10.4.1.1 step 5:
    // whatever receiver the call arrived with, the target sees [[BoundThis]].
    RootedArgs args(argc, argv);
    Rooted<Value> env{Value(envBits)};
    if (!env.get().isObject() ||
        env.get().asObject<HeapObjectHeader>()->flags != EnvHeader::kFlags) {
        fatal("internal: a bound function whose environment is not its binding cell");
    }
    (void)thisBits;
    Rooted<Value> target{readBoundSlot(env, BoundSlot::Target)};
    Rooted<Value> boundThis{readBoundSlot(env, BoundSlot::BoundThis)};
    Rooted<Value> boundArgs{readBoundSlot(env, BoundSlot::BoundArgs)};

    const uint32_t bound = boundArgs.get().asObject<ArrayHeader>()->length;
    // A RootedBlock, not a stack array: the callee may allocate before its
    // prologue copies, and this block was built by the runtime rather than by
    // generated code's rooted frame (rt_roots.h's RootedBlock header says
    // why that combination is the one that needs it).
    RootedBlock block(bound + args.count());
    for (uint32_t i = 0; i < bound; ++i) {
        block.set(i, boundArgs.get().asObject<ArrayHeader>()->getElem(i));
    }
    for (uint32_t i = 0; i < args.count(); ++i) block.set(bound + i, args[i]);
    return bronze_dynamic_call(target.get().rawBits(), boundThis.get().rawBits(),
                               bound + args.count(), block.data());
}

// "bound " + the target's name (20.2.3.2 step 4 via SetFunctionName's prefix),
// as the arena-interned header a FunctionHeader's `name` field must be.
// Memoized on the TARGET's name pointer — itself arena-interned and immortal —
// so `f.bind(x)` in a loop mints one arena string, not one per iteration.
// A bind chain still pays one per level ("bound f", "bound bound f", ...),
// which is as many distinct names as the program can observe.
StringHeader* boundName(const StringHeader* targetName) {
    static thread_local std::vector<std::pair<const StringHeader*, StringHeader*>> memo;
    for (const auto& entry : memo) {
        if (entry.first == targetName) return entry.second;
    }
    const std::string text = "bound " + rtUtf8Chars(targetName);
    StringHeader* tmp = StringHeader::createFromUTF8(rtHeap(), std::string_view(text));
    StringHeader* interned = StringHeader::internToArena(rtArena(), tmp);
    memo.emplace_back(targetName, interned);
    return interned;
}

}  // namespace

// 20.2.3.2. Reached as a member of any function through rtFunctionMethod's
// table (builtin_function.cpp), with the target as `this`.
uint64_t rtFunctionBindBuiltin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> target{Value(thisBits)};
    if (!target.get().isObject() ||
        target.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError(
                   "Function.prototype.bind called on a value that is not a function")
            .rawBits();
    }
    Rooted<Value> boundThis{args[0]};

    // [[BoundArguments]] as an ordinary array in the cell, so the payload scan
    // covers it and the trampoline reads it like any other array.
    const uint32_t boundCount = args.count() > 1 ? args.count() - 1 : 0;
    Rooted<Value> boundArgs{Value(bronze_create_array(boundCount))};
    for (uint32_t i = 0; i < boundCount; ++i) {
        Rooted<Value> v{args[i + 1]};
        boundArgs.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, v);
    }

    Rooted<Value> parent{Value::fromUndefined()};
    Rooted<Value> env{
        Value::fromObject(EnvHeader::create(rtHeap(), parent, BoundSlot::kBoundSlots))};
    // Slot writes allocate nothing, so one derivation of the cell serves all
    // three — each VALUE still read through its own root.
    EnvHeader* cell = env.get().asObject<EnvHeader>();
    cell->slotsData()[BoundSlot::Target] = target.get();
    cell->slotsData()[BoundSlot::BoundThis] = boundThis.get();
    cell->slotsData()[BoundSlot::BoundArgs] = boundArgs.get();

    // The same shape as bronze_create_function: allocate first, then read the
    // environment through its root — the allocation can collect.
    FunctionHeader* fn =
        FunctionHeader::create(rtHeap(), boundCallTrampoline, Value::fromUndefined(), 0);
    fn->env_record = env.get();
    fn->header.flags = HeapKind::Function;
    Rooted<Value> fnRoot{Value::fromObject(fn)};

    // `name` = "bound " + target name and `length` = max(0, target length −
    // bound count) — 20.2.3.2 steps 2-4 — but only when the target CARRIES the
    // pair. A native builtin records neither (rt_builtins.h's rtNativeFunction
    // says why), and a bound function over one inherits the same diagnosed
    // absence rather than two invented facts.
    const FunctionHeader* targetFn = target.get().asObject<FunctionHeader>();
    if (targetFn->name != nullptr) {
        const uint32_t targetLength = targetFn->length;
        StringHeader* named = boundName(targetFn->name);  // allocates
        FunctionHeader* live = fnRoot.get().asObject<FunctionHeader>();
        live->name = named;
        live->length = targetLength > boundCount ? targetLength - boundCount : 0;
    }
    return fnRoot.get().rawBits();
}

// The brand and the cell, for `bronze_construct`'s unwrapping. True only for a
// function whose code IS the trampoline; the three outputs are the cell's
// slots, read without allocating so the caller may hold them briefly before
// rooting.
bool rtBoundFunctionState(Value fn, Value& target, Value& boundThis, Value& boundArgs) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const FunctionHeader* header = fn.asObject<FunctionHeader>();
    if (header->code != boundCallTrampoline) return false;
    const Value env = header->env_record;
    if (!env.isObject() || env.asObject<HeapObjectHeader>()->flags != EnvHeader::kFlags) {
        fatal("internal: a bound function whose environment is not its binding cell");
    }
    const Value* slots = env.asObject<EnvHeader>()->slotsData();
    target = slots[BoundSlot::Target];
    boundThis = slots[BoundSlot::BoundThis];
    boundArgs = slots[BoundSlot::BoundArgs];
    return true;
}

}  // namespace bronze::runtime

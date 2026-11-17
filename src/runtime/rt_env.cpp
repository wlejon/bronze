// Environment records (closures' captured bindings): the helpers generated
// code calls to make one and to read and write a slot `depth` parent hops up,
// at `index`, with the temporal-dead-zone read of a `let`/`const`/`class`
// binding and the diagnostic of a failed inline access guard.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/env.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/profile.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

extern "C" {

uint64_t bronze_env_create(uint64_t parentBits, uint32_t slotCount) {
    recordHelperCall("bronze_env_create");
    Rooted<Value> parent{Value(parentBits)};
    EnvHeader* env = EnvHeader::create(rtHeap(), parent, slotCount);
    return Value::fromObject(env).rawBits();
}

static EnvHeader* resolveEnv(uint64_t envBits, uint32_t depth) {
    Value envVal(envBits);
    if (!envVal.isObject()) {
        fatal("environment access on a value that is not an environment record");
    }
    auto* env = envVal.asObject<EnvHeader>();
    if (env->header.flags != EnvHeader::kFlags) {
        fatal("environment access on a value that is not an environment record");
    }
    return env->ancestor(depth);
}

uint64_t bronze_env_get(uint64_t envBits, uint32_t depth, uint32_t index) {
    recordHelperCall("bronze_env_get");
    EnvHeader* env = resolveEnv(envBits, depth);
    if (index >= env->slotCount()) {
        fatal("environment slot index out of range (lowering bug)");
    }
    return env->slotsData()[index].rawBits();
}

static_assert(Value::fromUninitialized().rawBits() == BRONZE_ABI_UNINITIALIZED_BITS,
              "BRONZE_ABI_UNINITIALIZED_BITS in bronze_abi.h has drifted from the "
              "uninitialized-binding singleton");

// The same read, for a slot holding a `let`, `const` or `class` binding.
// ECMA-262 9.1.1.1.6 GetBindingValue: "if the binding for N in envRec is an
// uninitialized binding, throw a ReferenceError". The marker is the only thing
// that distinguishes the two states, and it can never be a program's value, so
// one compare against it IS the check.
//
// `keyIndex` is the module's interned name, so the message can say which
// binding without the backend carrying a string — the same arrangement
// `bronze_reference_error` uses.
uint64_t bronze_env_get_tdz(uint64_t envBits, uint32_t depth, uint32_t index,
                            uint32_t keyIndex) {
    recordHelperCall("bronze_env_get_tdz");
    const uint64_t bits = bronze_env_get(envBits, depth, index);
    if (bits != BRONZE_ABI_UNINITIALIZED_BITS) return bits;
    // A binding name reaching here can be a FLATTENED one — the module linker
    // renames a non-entry file's module scope to `modN.local`, and it can do
    // that safely precisely because a JavaScript identifier contains no dot. So
    // the text after the last dot is the name the program wrote, and a name
    // with no dot is already it. The message says what the source says;
    // `mod3.aLate` names a compilation strategy the programmer did not choose.
    const std::string& key = rtKeyString(keyIndex);
    const size_t dot = key.rfind('.');
    const std::string shown = dot == std::string::npos ? key : key.substr(dot + 1);
    return rtThrowReferenceError("Cannot access '" + shown + "' before initialization").rawBits();
}

// The inline path's access guard failed. Every question it asks is settled by
// the scope plan before the IL exists — the depth and the index are constants
// and a record's layout is fixed where it is created — so reaching here is a
// backend or lowering bug and there is no program state to recover. Walking
// the chain again is only so the diagnostic can name WHICH of the three
// questions the generated code answered no to.
// `fatal` is [[noreturn]], so every path out of this ends the process; the
// attribute is not repeated on the definition because the ABI header already
// declared the symbol without it.
void bronze_env_access_failed(uint64_t envBits, uint32_t depth, uint32_t index) {
    Value cur(envBits);
    for (uint32_t step = 0;; ++step) {
        if (!cur.isObject()) {
            fatal("environment access on a value that is not an environment record");
        }
        auto* env = cur.asObject<EnvHeader>();
        if (env->header.flags != EnvHeader::kFlags) {
            fatal("environment access on a value that is not an environment record");
        }
        if (step == depth) {
            if (index >= env->slotCount()) {
                fatal("environment slot index out of range (lowering bug)");
            }
            break;
        }
        cur = env->parent;
    }
    fatal("environment access guard failed on a well-formed record (backend bug)");
}

void bronze_env_set(uint64_t envBits, uint32_t depth, uint32_t index, uint64_t valBits) {
    recordHelperCall("bronze_env_set");
    EnvHeader* env = resolveEnv(envBits, depth);
    if (index >= env->slotCount()) {
        fatal("environment slot index out of range (lowering bug)");
    }
    env->slotsData()[index] = Value(valBits);
}

}  // extern "C"

}  // namespace bronze::runtime

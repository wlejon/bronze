#pragma once

#include <cstdint>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// The primitives-only shape generated code is entered through; declared
// in the pure-C ABI registry (src/abi/bronze_abi.h) so a C++ class type
// can never leak into it — see the header for the sret-shift rationale.
using NativeFunctionCode = bronze_fn_code;

struct FunctionHeader {
    HeapObjectHeader header;
    NativeFunctionCode code{nullptr};
    void* env_record{nullptr};
    uint32_t arity{0};

    static FunctionHeader* create(Heap& heap, NativeFunctionCode code, void* env_record = nullptr, uint32_t arity = 0);

    Value call(Value thisArg, uint32_t argc, Value* argv) const;
};

}  // namespace bronze

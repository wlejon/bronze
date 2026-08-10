#pragma once

#include <cstdint>
#include <vector>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

using NativeFunctionCode = Value (*)(Value thisArg, uint32_t argc, Value* argv);

struct FunctionHeader {
    HeapObjectHeader header;
    NativeFunctionCode code{nullptr};
    void* env_record{nullptr};
    uint32_t arity{0};

    static FunctionHeader* create(Heap& heap, NativeFunctionCode code, void* env_record = nullptr, uint32_t arity = 0);

    Value call(Value thisArg, uint32_t argc, Value* argv) const;
};

}  // namespace bronze

#pragma once

#include <cstdint>
#include <vector>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// ABI boundary with generated code: primitives only, u64 in / u64 out.
// `Value` (or any C++ class) must never appear in a signature that
// generated code calls or is called through — under the MSVC x64 ABI a
// class with user-defined constructors is returned via a hidden sret
// pointer in RCX, shifting every argument one register over versus the
// (i64, i32, ptr) -> i64 convention LLVM-emitted wrappers use.
using NativeFunctionCode = uint64_t (*)(uint64_t thisArgBits, uint32_t argc, const uint64_t* argvBits);

struct FunctionHeader {
    HeapObjectHeader header;
    NativeFunctionCode code{nullptr};
    void* env_record{nullptr};
    uint32_t arity{0};

    static FunctionHeader* create(Heap& heap, NativeFunctionCode code, void* env_record = nullptr, uint32_t arity = 0);

    Value call(Value thisArg, uint32_t argc, Value* argv) const;
};

}  // namespace bronze

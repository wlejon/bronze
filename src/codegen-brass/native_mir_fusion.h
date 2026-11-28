#pragma once

#include "il/il.h"

#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bronze::codegen {

// Attempts to populate `thunk` (`__bronze_native_<i>`) with the inlined Brass MIR
// body from a `registerNativeMir` registration when building an in-process module.
// Returns true if `thunk` was completely built (fast fused body + optional cold
// epoch-mismatch fallback); returns false with `thunk` empty/recreated so the
// caller falls back to the standard indirect thunk.
bool tryEmitMirNativeThunk(
    brass::Module& mod,
    brass::Function*& thunk,
    const std::string& importName,
    const std::string& importSignature,
    bool boolReturn,
    const std::vector<brass::Type>& paramTypes,
    brass::Type retType,
    const std::string& entrySymbol,
    const std::string& importsSymbol,
    int32_t slotOffset,
    bool perThread);

}  // namespace bronze::codegen

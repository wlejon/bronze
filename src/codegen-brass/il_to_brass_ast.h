#pragma once

#include "il/il.h"
#include <brass/il_translator/il_ast.hpp>
#include <string>
#include <vector>

namespace bronze::codegen {

// Lowers the IL module to brass's AST. Every `global.get` becomes a call to a
// per-key read thunk named `__bronze_global_read_k<keyIndex>`; the backend
// builds those thunks over a per-module cache array (see brass_backend.cpp).
// `globalReadKeys`, when given, receives the distinct key indices the module
// reads, in ascending order — each is one cache slot.
brass::il::BronzeModuleAST lowerToBrassAst(
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    std::vector<uint32_t>* globalReadKeys = nullptr
);

// The thunk name a `global.get` of `keyIndex` calls.
std::string globalReadThunkName(uint32_t keyIndex);

} // namespace bronze::codegen

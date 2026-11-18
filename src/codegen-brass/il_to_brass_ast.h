#pragma once

#include "il/il.h"
#include "codegen-brass/il2mir/il_ast.h"
#include <string>
#include <vector>

namespace bronze::codegen {

// Lowers the IL module to brass's AST. Every `global.get` becomes a call to a
// per-key read thunk named `__bronze_global_read_k<keyIndex>`; the backend
// builds those thunks over a per-module cache array (see brass_backend.cpp).
// `globalReadKeys`, when given, receives the distinct key indices the module
// reads, in ascending order — each is one cache slot.
// A module whose bodies are read on first need (an image's interpreted cold
// functions, cold_companion.h): per IL function, whether its body is
// deferred, and its CreateFunction instructions' callees and parameter
// counts in body order, what lowering reads of every body up front.
struct DeferredBodies {
    std::vector<uint8_t> deferred;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> createFuncs;
};

il2mir::BronzeModuleAST lowerToBrassAst(
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    std::vector<uint32_t>* globalReadKeys = nullptr,
    const DeferredBodies* deferred = nullptr
);

// One function as lowerToBrassAst converts it: `fn` is module function
// `fnIdx`, its body read since (a deferred body).
il2mir::BronzeFunction lowerFunctionToBrassAst(const il::Module& module, const il::Function& fn, size_t fnIdx,
                                               const std::vector<std::string>& uniqueNames);

// The thunk name a `global.get` of `keyIndex` calls.
std::string globalReadThunkName(uint32_t keyIndex);

} // namespace bronze::codegen

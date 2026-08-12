#pragma once

// The generated-code ABI expressed in LLVM terms. Every symbol compiled
// output links against is declared here, and only here, by rebinding the
// type tokens of the registry in src/abi/bronze_abi.h — so a signature
// drift between the C prototypes and the LLVM declarations is structurally
// impossible, which is the whole point of the registry (see that header for
// the sret-shift crash that made it a hard rule).
//
// The inline-cache table lives here too, because it is the one piece of *data*
// the generated object file owns on the ABI's behalf.

#include <cstdint>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "abi/bronze_abi.h"

namespace bronze::codegen_llvm {

// One llvm::Function* per entry in the ABI registry, named after the
// runtime symbol itself.
struct AbiFns {
#define BRONZE_ABI_FIELD(name, RET, PARAMS) llvm::Function* name;
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_FIELD)
#undef BRONZE_ABI_FIELD
};

// Likewise for the registry's data symbols.
struct AbiGlobals {
#define BRONZE_ABI_GLOBAL_FIELD(name, TYPE) llvm::GlobalVariable* name;
    BRONZE_ABI_GLOBALS(BRONZE_ABI_GLOBAL_FIELD)
#undef BRONZE_ABI_GLOBAL_FIELD
};

// Declares every registry symbol into `llvmModule`. Declarations only: the
// runtime owns every definition.
void declareAbiSymbols(llvm::Module& llvmModule, llvm::LLVMContext& ctx, AbiFns& fns,
                       AbiGlobals& globals);

// The module's inline-cache table: `siteCount` zero-initialized entries,
// private to this object file, one per property site lowering numbered.
// Null when the module has no property sites at all.
//
// Zero-initialized is load-bearing: a null `cached_shape` matches no real
// object, because ObjectHeader::create refuses to build an object without
// a shape. So a cold entry misses and falls into the helper, which fills
// it — there is no "is this entry valid" flag and none is needed.
llvm::GlobalVariable* createIcTable(llvm::Module& llvmModule, llvm::LLVMContext& ctx,
                                    uint32_t siteCount);

// Address of one entry in that table, as the `uint64_t*` the helpers take.
llvm::Value* icEntryPtr(llvm::IRBuilder<>& builder, llvm::GlobalVariable* icTable,
                        uint32_t icIndex);

}  // namespace bronze::codegen_llvm

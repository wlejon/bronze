#pragma once

// Property reads and writes in generated code, including the inlined
// inline-cache check generated code performs itself.

#include <cstdint>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

#include <string_view>

namespace bronze::codegen_llvm {

// Emits a property read and returns its i64 (NaN-boxed) result.
//
// `monomorphic` selects whether an inline IC check is generated.
// `keyStr` (when non-empty) enables compile-time index or length fast paths.
llvm::Value* emitPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                         llvm::GlobalVariable* icTable, llvm::Value* objBits, uint32_t keyIndex,
                         uint32_t icIndex, bool monomorphic, std::string_view keyStr = {});

// Property writes.
void emitPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::GlobalVariable* icTable,
                 llvm::Value* objBits, uint32_t keyIndex, llvm::Value* valBits, uint32_t icIndex,
                 bool strict, bool monomorphic = false, std::string_view keyStr = {});

}  // namespace bronze::codegen_llvm

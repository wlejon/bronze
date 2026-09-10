#pragma once

#include <llvm/IR/IRBuilder.h>

namespace bronze::codegen_llvm {

llvm::Value* emitConcatStep(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* lhs,
                            llvm::Value* rhs, llvm::Value* remaining);

llvm::Value* emitConcatEnd(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* val);

}  // namespace bronze::codegen_llvm

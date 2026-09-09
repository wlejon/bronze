#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "codegen-llvm/llvm_array_store_proof.h"
#include "codegen-llvm/llvm_static_slot.h"

namespace bronze::codegen_llvm {

using Globals = AbiGlobals;

llvm::Value* emitIndexPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                             const Globals& globals, const ModuleTables& tables,
                             llvm::Value* objBits, llvm::Value* objSlot,
                             uint32_t keyIndex, uint32_t icIndex,
                             bool monomorphic, StaticSite site,
                             std::string_view keyStr, uint32_t idx,
                             ReceiverProof* proof, ProofJoin* join,
                             bool holeRawSlot);

void emitIndexPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                     const Globals& globals, const ModuleTables& tables,
                     llvm::Value* objBits, llvm::Value* objSlot,
                     llvm::Value* valBits, uint32_t keyIndex,
                     uint32_t icIndex, bool monomorphic, StaticSite site,
                     std::string_view keyStr, uint32_t idx,
                     bool strict, ArrayStoreProof* proof, ProofJoin* join);

} // namespace bronze::codegen_llvm

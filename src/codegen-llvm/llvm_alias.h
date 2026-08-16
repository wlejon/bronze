#pragma once

#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>

namespace bronze::codegen_llvm {

struct ScopedAliasInfo {
    llvm::MDNode* taScopeList;
    llvm::MDNode* envScopeList;
};

inline ScopedAliasInfo getScopedAliasInfo(llvm::LLVMContext& ctx) {
    llvm::MDNode* domain = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "BronzeAliasDomain")});
    llvm::MDNode* taScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "TypedArrayData"), domain});
    llvm::MDNode* envScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "EnvRecordSlots"), domain});

    llvm::MDNode* taScopeList = llvm::MDNode::get(ctx, {taScope});
    llvm::MDNode* envScopeList = llvm::MDNode::get(ctx, {envScope});
    return {taScopeList, envScopeList};
}

inline void tagTypedArrayAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.taScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.envScopeList);
}

inline void tagEnvRecordAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.envScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.taScopeList);
}

}  // namespace bronze::codegen_llvm

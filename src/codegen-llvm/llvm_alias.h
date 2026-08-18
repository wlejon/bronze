#pragma once

#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>

namespace bronze::codegen_llvm {

// Three scoped-alias families, one domain. The claims are about DISJOINT
// BYTES, which is what makes each of them true unconditionally:
//
//  - TypedArrayData: the element bytes of a typed-array view — everything at
//    or past BRONZE_ABI_BUF_DATA_OFFSET in some buffer.
//  - EnvRecordSlots: the value slots of an environment record.
//  - TypedArrayViewLength: a view header's `length` word. An element store
//    can never change a view's length, and declaring that is what lets the
//    length load on every inline element path hoist out of a call-free
//    element loop DESPITE not being an invariant load. It must not be one:
//    `transfer` and a `resize` really do rewrite view lengths
//    (closeOrReopenViews in runtime/typed_array.cpp) — but only ever inside
//    a call, which clobbers, so the reload happens exactly when it must.
struct ScopedAliasInfo {
    llvm::MDNode* taScopeList;
    llvm::MDNode* envScopeList;
    llvm::MDNode* viewLenScopeList;
    llvm::MDNode* taNoaliasList;       // env + viewLen
    llvm::MDNode* envNoaliasList;      // ta + viewLen
    llvm::MDNode* viewLenNoaliasList;  // ta + env
};

inline ScopedAliasInfo getScopedAliasInfo(llvm::LLVMContext& ctx) {
    llvm::MDNode* domain = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "BronzeAliasDomain")});
    llvm::MDNode* taScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "TypedArrayData"), domain});
    llvm::MDNode* envScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "EnvRecordSlots"), domain});
    llvm::MDNode* viewLenScope =
        llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "TypedArrayViewLength"), domain});

    return {llvm::MDNode::get(ctx, {taScope}),
            llvm::MDNode::get(ctx, {envScope}),
            llvm::MDNode::get(ctx, {viewLenScope}),
            llvm::MDNode::get(ctx, {envScope, viewLenScope}),
            llvm::MDNode::get(ctx, {taScope, viewLenScope}),
            llvm::MDNode::get(ctx, {taScope, envScope})};
}

inline void tagTypedArrayAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.taScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.taNoaliasList);
}

inline void tagEnvRecordAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.envScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.envNoaliasList);
}

inline void tagViewLengthAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.viewLenScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.viewLenNoaliasList);
}

}  // namespace bronze::codegen_llvm

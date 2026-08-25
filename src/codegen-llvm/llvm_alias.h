#pragma once

#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace bronze::codegen_llvm {

// Six scoped-alias families under one domain ("BronzeAliasDomain"), named at
// the emission site that knows which one it is reading.
// The claims are about DISJOINT BYTES, which makes each of them true unconditionally:
//
//  - TypedArrayData: the element bytes of a typed-array view — everything at
//    or past BRONZE_ABI_BUF_DATA_OFFSET in some buffer (or external buffer).
//  - ArrayElementsData: the element Value slots of a JS Array (elemsObj->slots[1..]).
//  - ObjectPropertySlots: the inline/overflow property Value slots of plain objects.
//  - EnvRecordSlots: the Value slots of an environment record.
//  - TypedArrayViewLength: a view header's `length` word and buffer's `extbits`.
//  - ArrayHeaderFields: an Array header's `length`, `capacity`, `head`, `props`, `elems`.
//
// A SECOND set — the storage families below, one per stack frame and one per
// word of runtime bookkeeping — is assigned afterwards, by pointer provenance,
// in tagStackAndControlAccesses. That set also EXTENDS the six lists here,
// which is why nothing about them changes when it is added.
struct ScopedAliasInfo {
    llvm::MDNode* taScopeList;
    llvm::MDNode* arrElemScopeList;
    llvm::MDNode* objSlotScopeList;
    llvm::MDNode* envScopeList;
    llvm::MDNode* viewLenScopeList;
    llvm::MDNode* arrHdrScopeList;

    llvm::MDNode* taNoaliasList;
    llvm::MDNode* arrElemNoaliasList;
    llvm::MDNode* objSlotNoaliasList;
    llvm::MDNode* envNoaliasList;
    llvm::MDNode* viewLenNoaliasList;
    llvm::MDNode* arrHdrNoaliasList;
};

inline ScopedAliasInfo getScopedAliasInfo(llvm::LLVMContext& ctx) {
    llvm::MDNode* domain = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "BronzeAliasDomain")});
    llvm::MDNode* taScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "TypedArrayData"), domain});
    llvm::MDNode* arrElemScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "ArrayElementsData"), domain});
    llvm::MDNode* objSlotScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "ObjectPropertySlots"), domain});
    llvm::MDNode* envScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "EnvRecordSlots"), domain});
    llvm::MDNode* viewLenScope =
        llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "TypedArrayViewLength"), domain});
    llvm::MDNode* arrHdrScope = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "ArrayHeaderFields"), domain});

    llvm::MDNode* taScopeList = llvm::MDNode::get(ctx, {taScope});
    llvm::MDNode* arrElemScopeList = llvm::MDNode::get(ctx, {arrElemScope});
    llvm::MDNode* objSlotScopeList = llvm::MDNode::get(ctx, {objSlotScope});
    llvm::MDNode* envScopeList = llvm::MDNode::get(ctx, {envScope});
    llvm::MDNode* viewLenScopeList = llvm::MDNode::get(ctx, {viewLenScope});
    llvm::MDNode* arrHdrScopeList = llvm::MDNode::get(ctx, {arrHdrScope});

    return {
        taScopeList,
        arrElemScopeList,
        objSlotScopeList,
        envScopeList,
        viewLenScopeList,
        arrHdrScopeList,
        llvm::MDNode::get(ctx, {arrElemScope, objSlotScope, envScope, viewLenScope, arrHdrScope}),
        llvm::MDNode::get(ctx, {taScope, objSlotScope, envScope, viewLenScope, arrHdrScope}),
        llvm::MDNode::get(ctx, {taScope, arrElemScope, envScope, viewLenScope, arrHdrScope}),
        llvm::MDNode::get(ctx, {taScope, arrElemScope, objSlotScope, viewLenScope, arrHdrScope}),
        llvm::MDNode::get(ctx, {taScope, arrElemScope, objSlotScope, envScope, arrHdrScope}),
        llvm::MDNode::get(ctx, {taScope, arrElemScope, objSlotScope, envScope, viewLenScope}),
    };
}

inline void tagTypedArrayAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.taScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.taNoaliasList);
}

inline void tagArrayElementsAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.arrElemScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.arrElemNoaliasList);
}

inline void tagObjectSlotAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.objSlotScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.objSlotNoaliasList);
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

inline void tagArrayHeaderAccess(llvm::Instruction* inst, llvm::LLVMContext& ctx) {
    auto alias = getScopedAliasInfo(ctx);
    inst->setMetadata(llvm::LLVMContext::MD_alias_scope, alias.arrHdrScopeList);
    inst->setMetadata(llvm::LLVMContext::MD_noalias, alias.arrHdrNoaliasList);
}

// The STORAGE families, assigned by POINTER PROVENANCE over the whole emitted
// module, after emission and before optimization. An access is claimed only
// when its underlying object is positively one of:
//
//   - an `alloca` in the compiled function                      → StackFrame
//   - a word of the per-thread ABI block, reached through a
//     `bronze_tls_block_addr` call or the module-local cache
//     `cacheTlsFetches` rewrote it into                         → TlsWord.<n>
//   - one of the module's own tables (an internal GlobalVariable
//     off the known list)                                       → ModuleTables
//
// Anything else — every pointer that came out of an `inttoptr` of a Value
// payload, which is the whole JS heap — is left exactly as its emitter tagged
// it, and an access that already carries a scope is never retagged.
//
// WHY THIS PASS EXISTS. A Dynamic def STORES its GC root slot, and that store
// carried no metadata: escaped stack memory may-aliases everything, so every
// cached control word after it had to be re-loaded and every guard over one
// re-tested. That is what made the same `pending` compare survive eight times
// in one inlined `Matrix4.multiplyMatrices`. The claims are the strongest kind
// — bytes of one thread's stack, bytes of one TLS word, bytes of one module
// table, and the JS heap are four disjoint regions, unconditionally — and the
// TLS block is split PER WORD because the frame link and the exception cell
// are adjacent, and after inlining two copies of a body reach them through
// two unrelated phis that BasicAA cannot relate.
//
// `tlsFn` is the ABI accessor; pass the same one `cacheTlsFetches` was given.
void tagStackAndControlAccesses(llvm::Module& llvmModule, llvm::Function* tlsFn);

}  // namespace bronze::codegen_llvm

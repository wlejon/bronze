#include "codegen-llvm/llvm_alias.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "abi/bronze_abi.h"

namespace bronze::codegen_llvm {

namespace {

// The module's own tables (llvm_abi.cpp createTable, llvm_func.cpp). Every one
// of them holds RUNTIME BOOKKEEPING — inline-cache words, a family stamp base,
// a cell that holds a Value — and never the bytes of a JS object: a cell holds
// the Value that names an object, and the object's own slots live in the heap
// the collector owns. Spelled out rather than derived from linkage, because
// "every internal global" would also sweep in one a later change hands to the
// runtime as storage. One scope covers all of them: two distinct globals are
// already NoAlias to BasicAA, and scoped-noalias only ever adds to that.
constexpr std::array<std::string_view, 12> kControlTables{
    "__bronze_family_base",   "__bronze_family_classes",  "__bronze_family_fields",
    "__bronze_fn_slots",      "__bronze_global_cache",    "__bronze_ic_table",
    "__bronze_key_map",       "__bronze_method_ic_sites", "__bronze_module_env",
    "__bronze_static_shapes", "__bronze_template_slots",  "__bronze_tls_block_cache",
};

// Every field of bronze_tls_block is one 8-byte word (abi/bronze_abi_tls.h),
// so a word index is the whole identity of a TLS field and the last offset
// bounds the block.
constexpr unsigned kTlsWords = (BRONZE_TLS_TRUTHY_INLINE_ENABLED_OFF / 8) + 1;

bool isControlTable(const llvm::GlobalVariable* gv) {
    llvm::StringRef name = gv->getName();
    for (std::string_view known : kControlTables) {
        if (name == llvm::StringRef(known.data(), known.size())) return true;
    }
    return false;
}

// Which storage a pointer names, and — for the TLS block — which word of it.
struct Provenance {
    enum class Kind { None, Stack, Tables, TlsWord } kind = Kind::None;
    unsigned word = 0;
};

// Walk to the object a pointer is an offset into, accumulating the constant
// byte offset. `inttoptr` is NOT walked through and is where the walk stops,
// which is what keeps the whole JS heap — every pointer that came out of a
// Value payload — out of every family here. A non-constant index gives up on
// the offset, which only matters for the TLS case.
const llvm::Value* underlying(const llvm::Value* v, const llvm::DataLayout& dl,
                              uint64_t& offset, bool& offsetKnown) {
    offset = 0;
    offsetKnown = true;
    for (unsigned step = 0; step < 12; ++step) {
        const llvm::Value* stripped = v->stripPointerCasts();
        const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(stripped);
        if (!gep) return stripped;
        llvm::APInt delta(dl.getIndexTypeSizeInBits(gep->getType()), 0);
        if (gep->accumulateConstantOffset(dl, delta)) {
            offset += delta.getZExtValue();
        } else {
            offsetKnown = false;
        }
        v = gep->getPointerOperand();
    }
    return v;
}

Provenance classify(const llvm::Value* ptr, const llvm::DataLayout& dl,
                    const llvm::Function* tlsFn, unsigned depth = 0) {
    uint64_t offset = 0;
    bool offsetKnown = true;
    const llvm::Value* base = underlying(ptr, dl, offset, offsetKnown);

    auto tlsWord = [&]() -> Provenance {
        if (!offsetKnown || (offset % 8) != 0 || (offset / 8) >= kTlsWords) return {};
        return {Provenance::Kind::TlsWord, static_cast<unsigned>(offset / 8)};
    };

    if (llvm::isa<llvm::AllocaInst>(base)) return {Provenance::Kind::Stack, 0};
    if (const auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(base)) {
        return isControlTable(gv) ? Provenance{Provenance::Kind::Tables, 0} : Provenance{};
    }
    // The per-thread ABI block: the accessor's result, or the cache load
    // `cacheTlsFetches` rewrote it into. That rewrite leaves a two-way phi of
    // (cached, freshly fetched) at the top of the entry block, so the phi is
    // the shape every TLS field reads through in an optimized module.
    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(base)) {
        return call->getCalledOperand() == tlsFn ? tlsWord() : Provenance{};
    }
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(base)) {
        uint64_t innerOffset = 0;
        bool innerKnown = true;
        const auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(
            underlying(load->getPointerOperand(), dl, innerOffset, innerKnown));
        // Only the TLS cache: every other table load yields a Value or a shape,
        // and what it points at is the heap.
        if (!gv || gv->getName() != "__bronze_tls_block_cache") return {};
        return tlsWord();
    }
    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(base); phi && depth == 0 && offsetKnown) {
        // A phi is classified only when EVERY incoming value agrees, and the
        // offset walked to it is then the field. One unclassifiable edge
        // leaves the access untagged, which is the answer the pass had before
        // it looked.
        Provenance agreed;
        for (const llvm::Value* in : phi->incoming_values()) {
            uint64_t inOffset = 0;
            bool inKnown = true;
            const llvm::Value* inBase = underlying(in, dl, inOffset, inKnown);
            if (!inKnown || inOffset != 0) return {};
            Provenance p = classify(inBase, dl, tlsFn, depth + 1);
            if (p.kind == Provenance::Kind::None) return {};
            if (agreed.kind != Provenance::Kind::None &&
                (agreed.kind != p.kind || agreed.word != p.word)) {
                return {};
            }
            agreed = p;
        }
        // The incoming values were classified at offset zero; this access's own
        // offset off the phi is what picks the word.
        if (agreed.kind == Provenance::Kind::TlsWord) return tlsWord();
        return agreed;
    }
    return {};
}

// The storage scopes, in one fixed order: StackFrame, ModuleTables, then one
// per TLS word. Built once per module.
struct StorageScopes {
    llvm::SmallVector<llvm::Metadata*, kTlsWords + 2> all;
    llvm::MDNode* heapNoalias;  // every storage scope, for extending the heap lists

    llvm::MDNode* scopeListFor(llvm::LLVMContext& ctx, size_t i) const {
        return llvm::MDNode::get(ctx, {all[i]});
    }
    llvm::MDNode* noaliasFor(llvm::LLVMContext& ctx, size_t i,
                             llvm::ArrayRef<llvm::Metadata*> heapScopes) const {
        llvm::SmallVector<llvm::Metadata*, kTlsWords + 8> others(heapScopes.begin(),
                                                                heapScopes.end());
        for (size_t k = 0; k < all.size(); ++k) {
            if (k != i) others.push_back(all[k]);
        }
        return llvm::MDNode::get(ctx, others);
    }
};

size_t scopeIndexOf(const Provenance& p) {
    switch (p.kind) {
        case Provenance::Kind::Stack: return 0;
        case Provenance::Kind::Tables: return 1;
        case Provenance::Kind::TlsWord: return 2 + p.word;
        case Provenance::Kind::None: break;
    }
    return 0;
}

}  // namespace

void tagStackAndControlAccesses(llvm::Module& llvmModule, llvm::Function* tlsFn) {
    llvm::LLVMContext& ctx = llvmModule.getContext();
    const llvm::DataLayout& dl = llvmModule.getDataLayout();
    llvm::MDNode* domain =
        llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "BronzeAliasDomain")});

    StorageScopes scopes;
    auto addScope = [&](const std::string& name) {
        scopes.all.push_back(llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, name), domain}));
    };
    addScope("StackFrame");
    addScope("ModuleTables");
    for (unsigned w = 0; w < kTlsWords; ++w) addScope("TlsWord." + std::to_string(w));
    scopes.heapNoalias = llvm::MDNode::get(ctx, scopes.all);

    // The six heap scopes, taken apart from the lists the emitters built so the
    // storage lists name exactly the same MDNodes.
    const ScopedAliasInfo heap = getScopedAliasInfo(ctx);
    const llvm::MDNode* heapLists[] = {heap.taScopeList,      heap.arrElemScopeList,
                                       heap.objSlotScopeList, heap.envScopeList,
                                       heap.viewLenScopeList, heap.arrHdrScopeList};
    llvm::SmallVector<llvm::Metadata*, 6> heapScopes;
    for (const llvm::MDNode* list : heapLists) heapScopes.push_back(list->getOperand(0));

    for (llvm::Function& f : llvmModule) {
        if (f.isDeclaration()) continue;
        for (llvm::BasicBlock& bb : f) {
            for (llvm::Instruction& inst : bb) {
                const llvm::Value* ptr = nullptr;
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                    ptr = load->getPointerOperand();
                } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                    ptr = store->getPointerOperand();
                } else {
                    continue;
                }
                if (llvm::MDNode* existing =
                        inst.getMetadata(llvm::LLVMContext::MD_alias_scope)) {
                    // A heap family the emitter named. It keeps its scope; what
                    // it gains is the claim that no storage family aliases it.
                    (void)existing;
                    llvm::MDNode* prior = inst.getMetadata(llvm::LLVMContext::MD_noalias);
                    llvm::MDNode* extended =
                        prior ? llvm::MDNode::concatenate(prior, scopes.heapNoalias)
                              : scopes.heapNoalias;
                    inst.setMetadata(llvm::LLVMContext::MD_noalias, extended);
                    continue;
                }
                Provenance p = classify(ptr, dl, tlsFn);
                if (p.kind == Provenance::Kind::None) continue;
                const size_t idx = scopeIndexOf(p);
                inst.setMetadata(llvm::LLVMContext::MD_alias_scope,
                                 scopes.scopeListFor(ctx, idx));
                inst.setMetadata(llvm::LLVMContext::MD_noalias,
                                 scopes.noaliasFor(ctx, idx, heapScopes));
            }
        }
    }
}

}  // namespace bronze::codegen_llvm

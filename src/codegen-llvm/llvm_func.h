#pragma once

// One IL function's body, emitted into one LLVM function.
//
// The state below is shared by every instruction of that body — the GC root
// frame, the block phis of the block-argument SSA, and the SSA-value table — so
// it is held together rather than threaded through a dozen parameters.

#include <cstdint>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

#include "codegen-llvm/llvm_abi.h"
#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::codegen_llvm {

// The LLVM type an IL type lowers to. Null for a type with no mapping, which
// the caller reports against the construct that produced it.
llvm::Type* mapILType(il::Type type, llvm::LLVMContext& ctx);

class FunctionEmitter {
public:
    struct Context {
        llvm::LLVMContext& ctx;
        const il::Module& module;
        const AbiFns& abi;
        // The tables this object file owns (llvm_abi.h): the IC sites, the key
        // remap, and the two module-local caches.
        const ModuleTables& tables;
        // Indexed by IL function index: the typed entry point, and the
        // uniform-convention wrapper that adapts to it.
        const std::vector<llvm::Function*>& entries;
        const std::vector<llvm::Function*>& wrappers;
        DiagnosticSink& diags;
        // Whether ANY function in the module reads `new.target`. The inline
        // `new` fast path skips the helper's NewTargetScope push, which is
        // observable through bronze_get_new_target and nothing else — so one
        // mention anywhere keeps every construct site on the helper.
        bool moduleHasNewTarget;
    };

    FunctionEmitter(const Context& shared, const il::Function& func, llvm::Function* llvmFunc);

    // Emits every block of the function. False on a diagnosed error.
    bool emit();

private:
    void planRootFrame();
    void emitPrologue();
    void createBlockPhis();
    void emitModuleInit();
    void emitFunctionSourceTables();
    bool emitBlock(size_t blockIndex);
    bool emitInstruction(const il::Instruction& inst);

    // Instruction families. Each returns false only after diagnosing.
    bool emitTerminator(const il::Instruction& inst);
    bool emitRuntimeOp(const il::Instruction& inst);
    bool emitDynamicCall(const il::Instruction& inst);
    bool emitMethodCall(const il::Instruction& inst);
    // The guarded direct edge in front of it, or false when this site has none.
    bool emitMethodCallDirect(const il::Instruction& inst, llvm::Value* thisVal, uint32_t argc);
    bool emitMethodCallSpread(const il::Instruction& inst);
    bool emitConstruct(const il::Instruction& inst);
    bool emitCall(const il::Instruction& inst);
    bool emitArithmetic(const il::Instruction& inst);
    // The private-element family (llvm_private.cpp). Its own unit because it
    // is one mechanism with six instructions, not six unrelated helpers.
    bool emitPrivateOp(const il::Instruction& inst);

    // Reloads a Dynamic value from its root slot at the point of use: if
    // anything collected since the def, the slot was forwarded and the SSA
    // register was not.
    void reload(il::ValueId id);
    llvm::Value* slotAddr(uint32_t slot);
    // The ADDRESS of an operand's root slot, for code that has to re-read the
    // value after a call rather than before it. Null when the value has no slot
    // (it is not Dynamic, so nothing can move it, and there is nothing to
    // reload) — a caller that needs a live pointer must treat that as "cannot".
    llvm::Value* rootSlotAddrOf(const il::Instruction& inst, size_t index);
    // Fills the frame's argv region and returns a pointer to it, or a null
    // pointer constant when the call takes no arguments. `first` is the index
    // of the first argument operand.
    llvm::Value* emitArgv(const il::Instruction& inst, size_t first, uint32_t argc, bool& ok);

    // The pending-exception cell test, emitted after any instruction
    // `il::canThrow` admits: load the cell, compare it against "nothing
    // pending", and branch to this block's handler or out of the function.
    // Splits the current LLVM block, so everything after it in the same IL
    // block is emitted into the continuation.
    void emitExceptionCheck(size_t blockIndex);
    // Where an unwind from `blockIndex` goes: the IL handler this block
    // names, or the function's single unwind block, created on first demand.
    llvm::BasicBlock* unwindTargetFor(size_t blockIndex);
    // Pops the GC root frame and returns — byte-for-byte what `emitTerminator`
    // does before an ordinary `ret`, which is the property that makes the whole
    // mechanism sound with respect to rooting.
    llvm::BasicBlock* functionUnwindBlock();
    void popRootFrame();

    // An operand as an LLVM value, diagnosed as `what` when it has no def.
    llvm::Value* operand(const il::Instruction& inst, size_t index, const char* what);
    bool require(bool condition, const char* message);

    static constexpr uint32_t kNoSlot = UINT32_MAX;

    const Context& shared_;
    const il::Function& func_;
    llvm::Function* llvmFunc_;
    llvm::IRBuilder<> builder_;

    llvm::Type* i64Ty_;
    llvm::Type* ptrTy_;

    std::vector<llvm::BasicBlock*> blocks_;
    std::vector<std::vector<llvm::PHINode*>> blockPhis_;
    std::vector<llvm::Value*> values_;
    // The key a PropGet result was read by (UINT32_MAX otherwise): what lets
    // a DynamicCall recognize `Math.sqrt(x)`-shaped callees and emit the
    // code-pointer-guarded direct dispatch (llvm_math.h). Provenance only —
    // soundness lives entirely in the emitted guard.
    std::vector<uint32_t> propGetKey_;
    // The IL function index a FunctionRef result was read by (UINT32_MAX otherwise):
    // what lets DynamicCall and Construct invoke the known wrapper directly.
    std::vector<uint32_t> funcRefIndex_;

    std::vector<uint32_t> slotOf_;
    uint32_t argvBase_ = 0;
    uint32_t frameSlots_ = 0;
    // A dedicated root slot for the inline `new` fast path's fresh instance,
    // live only across each site's constructor call — every site shares it,
    // exactly as the argv region is shared. kNoSlot when the function has no
    // Construct or the module's `new.target` use keeps the path off.
    uint32_t constructSelfSlot_ = kNoSlot;
    // This function's view of the per-thread ABI block: field addresses off
    // the one bronze_tls_block_addr() call bound at the top of the entry
    // block (llvm_abi.h, bindTlsBlock).
    AbiGlobals globals_;
    llvm::StructType* frameTy_ = nullptr;
    llvm::Value* framePtr_ = nullptr;
    llvm::Value* slotsBase_ = nullptr;
    llvm::BasicBlock* unwindBlock_ = nullptr;
    // The IL block being emitted, which a terminator needs in order to find
    // its handler: `builder_.GetInsertBlock()` may be a continuation an
    // inlined guard or a cell test split off, and a continuation is not an
    // IL block at all.
    size_t currentILBlock_ = 0;
    // Whether this module drops the environment-record ACCESS guards
    // (llvm_env.h, `envAccessGuardsElided`). A property of the invocation, so
    // it is read once rather than per env instruction.
    bool envGuardsElided_ = false;
};

}  // namespace bronze::codegen_llvm

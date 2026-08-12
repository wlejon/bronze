#include "codegen-llvm/llvm_abi.h"

#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

// The table is an array of i64 rather than of a named struct type: an IC
// entry is a whole number of words, the helpers take `uint64_t*`, and the
// field offsets the fast path reads are word-aligned constants in the ABI
// header. A struct type here would add a second, drifting description of a
// layout the ABI already pins.
//
// Generated code reads words 0 and 1 and never word 2 — the inline path is
// depth 0 only, and the epoch that word 2 holds is what makes a DEPTH > 0 entry
// sound, which is the helper's business. The stride is derived from the ABI
// constant rather than written as a literal, so growing the entry again stays
// one edit in one file.
static constexpr unsigned kIcEntryWords = BRONZE_ABI_IC_ENTRY_SIZE / sizeof(uint64_t);
static_assert(BRONZE_ABI_IC_ENTRY_SIZE % sizeof(uint64_t) == 0,
              "the IC table is emitted as i64 words, so an entry must be a whole number of them");
static_assert(BRONZE_ABI_IC_SHAPE_OFFSET == 0, "word 0 of an entry is the cached shape");
static_assert(BRONZE_ABI_IC_SLOTWORD_OFFSET == 8, "word 1 of an entry is (depth << 32) | slot");
static_assert(BRONZE_ABI_IC_EPOCH_OFFSET == 16, "word 2 of an entry is the fill epoch");

void declareAbiSymbols(llvm::Module& llvmModule, llvm::LLVMContext& ctx, AbiFns& fns,
                       AbiGlobals& globals) {
    auto getOrDeclareFunc = [&](const char* name, llvm::FunctionType* fty) -> llvm::Function* {
        llvm::Function* fn = llvmModule.getFunction(name);
        if (!fn) {
            fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, name, &llvmModule);
        }
        return fn;
    };

#define BRONZE_ABI_U64    llvm::Type::getInt64Ty(ctx)
#define BRONZE_ABI_U32    llvm::Type::getInt32Ty(ctx)
#define BRONZE_ABI_I32    llvm::Type::getInt32Ty(ctx)
#define BRONZE_ABI_F64    llvm::Type::getDoubleTy(ctx)
#define BRONZE_ABI_BOOL   llvm::Type::getInt1Ty(ctx)
#define BRONZE_ABI_CSTR   llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_PU64   llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_MU64   llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_FRAMEPTR llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_FNPTR  llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_VOID   llvm::Type::getVoidTy(ctx)
#define BRONZE_ABI_NOARGS
#define BRONZE_ABI_UNPAREN(...) __VA_ARGS__

#define BRONZE_ABI_LLVM_DECLARE(name, RET, PARAMS) \
    fns.name = getOrDeclareFunc(#name, \
        llvm::FunctionType::get(RET, {BRONZE_ABI_UNPAREN PARAMS}, false));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_LLVM_DECLARE)
#undef BRONZE_ABI_LLVM_DECLARE

#define BRONZE_ABI_LLVM_DECLARE_GLOBAL(name, TYPE)                                     \
    globals.name = new llvm::GlobalVariable(llvmModule, TYPE, /*isConstant=*/false,    \
                                            llvm::GlobalValue::ExternalLinkage,        \
                                            /*Initializer=*/nullptr, #name);
    BRONZE_ABI_GLOBALS(BRONZE_ABI_LLVM_DECLARE_GLOBAL)
#undef BRONZE_ABI_LLVM_DECLARE_GLOBAL

#undef BRONZE_ABI_UNPAREN
#undef BRONZE_ABI_U64
#undef BRONZE_ABI_U32
#undef BRONZE_ABI_I32
#undef BRONZE_ABI_F64
#undef BRONZE_ABI_BOOL
#undef BRONZE_ABI_CSTR
#undef BRONZE_ABI_PU64
#undef BRONZE_ABI_MU64
#undef BRONZE_ABI_FRAMEPTR
#undef BRONZE_ABI_FNPTR
#undef BRONZE_ABI_VOID
#undef BRONZE_ABI_NOARGS
}

llvm::GlobalVariable* createIcTable(llvm::Module& llvmModule, llvm::LLVMContext& ctx,
                                    uint32_t siteCount) {
    if (siteCount == 0) return nullptr;
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::ArrayType* tableTy =
        llvm::ArrayType::get(i64Ty, static_cast<uint64_t>(siteCount) * kIcEntryWords);
    // Internal linkage: the table belongs to this object file, and its
    // address never crosses a module boundary — generated code passes entry
    // pointers to the helpers and nothing else ever names it.
    auto* table = new llvm::GlobalVariable(llvmModule, tableTy, /*isConstant=*/false,
                                           llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantAggregateZero::get(tableTy),
                                           "__bronze_ic_table");
    table->setAlignment(llvm::Align(8));
    return table;
}

llvm::Value* icEntryPtr(llvm::IRBuilder<>& builder, llvm::GlobalVariable* icTable,
                        uint32_t icIndex) {
    return builder.CreateConstInBoundsGEP2_32(icTable->getValueType(), icTable, 0,
                                              icIndex * kIcEntryWords,
                                              "ic" + std::to_string(icIndex));
}

}  // namespace bronze::codegen_llvm

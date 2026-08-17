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

// The fn-singleton table is i64 words for the same reason as the IC table: the
// helper takes `uint64_t*` and the two field offsets are ABI constants.
static constexpr unsigned kFnSlotWords = BRONZE_ABI_FNSLOT_SIZE / sizeof(uint64_t);
static_assert(BRONZE_ABI_FNSLOT_SIZE % sizeof(uint64_t) == 0,
              "the fn-singleton table is emitted as i64 words, so an entry must be whole ones");
static_assert(BRONZE_ABI_FNSLOT_CODE_OFFSET == 0, "word 0 of a slot is the code pointer");
static_assert(BRONZE_ABI_FNSLOT_VALUE_OFFSET == 8, "word 1 of a slot is the Value");

void declareAbiSymbols(llvm::Module& llvmModule, llvm::LLVMContext& ctx, AbiFns& fns) {
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
#define BRONZE_ABI_TLSPTR llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_FNPTR  llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_VOID   llvm::Type::getVoidTy(ctx)
#define BRONZE_ABI_NOARGS
#define BRONZE_ABI_UNPAREN(...) __VA_ARGS__

#define BRONZE_ABI_LLVM_DECLARE(name, RET, PARAMS) \
    fns.name = getOrDeclareFunc(#name, \
        llvm::FunctionType::get(RET, {BRONZE_ABI_UNPAREN PARAMS}, false)); \
    fns.name->addFnAttr(llvm::Attribute::NoUnwind);
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_LLVM_DECLARE)
#undef BRONZE_ABI_LLVM_DECLARE

    // The TLS-block accessor returns an address that is a per-thread
    // constant: readnone + willreturn is what lets LLVM fold a function's
    // fetch away when nothing uses it and CSE it when something does.
    fns.bronze_tls_block_addr->setDoesNotAccessMemory();
    fns.bronze_tls_block_addr->addFnAttr(llvm::Attribute::WillReturn);

#undef BRONZE_ABI_UNPAREN
#undef BRONZE_ABI_U64
#undef BRONZE_ABI_U32
#undef BRONZE_ABI_I32
#undef BRONZE_ABI_F64
#undef BRONZE_ABI_BOOL
#undef BRONZE_ABI_CSTR
#undef BRONZE_ABI_PU64
#undef BRONZE_ABI_MU64
#undef BRONZE_ABI_TLSPTR
#undef BRONZE_ABI_FNPTR
#undef BRONZE_ABI_VOID
#undef BRONZE_ABI_NOARGS
}

AbiGlobals bindTlsBlock(llvm::IRBuilder<>& builder, const AbiFns& fns) {
    llvm::Value* base = builder.CreateCall(fns.bronze_tls_block_addr, {}, "tls");
    llvm::Type* i8Ty = builder.getInt8Ty();
    auto field = [&](uint64_t off, const char* name) -> llvm::Value* {
        if (off == 0) return base;
        return builder.CreateConstInBoundsGEP1_64(i8Ty, base, off, name);
    };
    AbiGlobals g;
    g.bronze_gc_frame_top = field(BRONZE_TLS_FRAME_TOP_OFF, "tls.frame_top");
    g.bronze_exception_cell = field(BRONZE_TLS_EXCEPTION_CELL_OFF, "tls.exc");
    g.bronze_proto_epoch = field(BRONZE_TLS_PROTO_EPOCH_OFF, "tls.epoch");
    g.bronze_alloc_cursor = field(BRONZE_TLS_ALLOC_CURSOR_OFF, "tls.cursor");
    g.bronze_alloc_limit = field(BRONZE_TLS_ALLOC_LIMIT_OFF, "tls.limit");
    g.bronze_plain_shape = field(BRONZE_TLS_PLAIN_SHAPE_OFF, "tls.shape");
    g.bronze_inline_call_enabled = field(BRONZE_TLS_INLINE_CALL_ENABLED_OFF, "tls.call_on");
    g.bronze_array_method_ic_enabled =
        field(BRONZE_TLS_ARRAY_METHOD_IC_ENABLED_OFF, "tls.arric_on");
    g.bronze_inline_overflow_set_enabled =
        field(BRONZE_TLS_INLINE_OVERFLOW_SET_ENABLED_OFF, "tls.ovset_on");
    g.bronze_inline_accessor_enabled =
        field(BRONZE_TLS_INLINE_ACCESSOR_ENABLED_OFF, "tls.acc_on");
    g.bronze_array_method_tbl = field(BRONZE_TLS_ARRAY_METHOD_TBL_OFF, "tls.arrtbl");
    return g;
}

namespace {

// Internal linkage on every one of these: a table belongs to its object file
// and its address never crosses a module boundary — generated code passes cell
// pointers to the helpers and nothing else ever names it. It is also what lets
// two compiled modules link into one image, which external linkage here would
// turn into a duplicate-symbol error per table.
llvm::GlobalVariable* createTable(llvm::Module& llvmModule, llvm::Type* ty,
                                  llvm::Constant* init, const char* name, unsigned align) {
    auto* table = new llvm::GlobalVariable(llvmModule, ty, /*isConstant=*/false,
                                           llvm::GlobalValue::InternalLinkage, init, name);
    table->setAlignment(llvm::Align(align));
    return table;
}

// One module-local global-cache slot per DISTINCT key a `global.get` names,
// rather than one per key in the pool. The pool carries every property name in
// the program and a program names a couple of dozen globals, so indexing the
// cache by key would put thousands of permanently-undefined cells in the
// object's data and walk every one of them at every collection.
//
// Assigned in IL order — functions, then blocks, then instructions — so the
// table's layout is a function of the module and nothing else.
std::vector<uint32_t> assignGlobalCacheSlots(const il::Module& module, uint32_t& countOut) {
    std::vector<uint32_t> slotOf(module.keyConstants.size(), ModuleTables::kNoGlobalCacheSlot);
    uint32_t next = 0;
    for (const auto& func : module.functions) {
        for (const auto& block : func.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != il::Op::GlobalGet) continue;
                if (inst.keyIndex >= slotOf.size()) continue;
                if (slotOf[inst.keyIndex] != ModuleTables::kNoGlobalCacheSlot) continue;
                slotOf[inst.keyIndex] = next++;
            }
        }
    }
    countOut = next;
    return slotOf;
}

}  // namespace

ModuleTables createModuleTables(llvm::Module& llvmModule, llvm::LLVMContext& ctx,
                                const il::Module& module) {
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    ModuleTables tables;
    tables.keyCount = static_cast<uint32_t>(module.keyConstants.size());
    tables.fnSlotCount = static_cast<uint32_t>(module.functions.size());
    tables.globalCacheSlotOf = assignGlobalCacheSlots(module, tables.globalCacheCount);

    if (module.icSiteCount > 0) {
        llvm::ArrayType* ty = llvm::ArrayType::get(
            i64Ty, static_cast<uint64_t>(module.icSiteCount) * kIcEntryWords);
        tables.icTable = createTable(llvmModule, ty, llvm::ConstantAggregateZero::get(ty),
                                     "__bronze_ic_table", 8);
    }

    if (tables.keyCount > 0) {
        llvm::ArrayType* ty = llvm::ArrayType::get(i32Ty, tables.keyCount);
        tables.keyMap = createTable(llvmModule, ty, llvm::ConstantAggregateZero::get(ty),
                                    "__bronze_key_map", 4);
    }

    if (tables.globalCacheCount > 0) {
        // Undefined and not zero: `undefined` is what the fast path reads as
        // "unfilled", and zero is the double 0.0. A zeroed cache would answer
        // every global with 0 rather than falling into the helper.
        llvm::ArrayType* ty = llvm::ArrayType::get(i64Ty, tables.globalCacheCount);
        std::vector<uint64_t> undef(tables.globalCacheCount, BRONZE_ABI_UNDEFINED_BITS);
        tables.globalCache =
            createTable(llvmModule, ty, llvm::ConstantDataArray::get(ctx, undef),
                        "__bronze_global_cache", 8);
    }

    // Undefined for the same reason the global cache is: zero is the double
    // 0.0, and the module env is read before it can be proven set.
    tables.moduleEnv = createTable(llvmModule, i64Ty,
                                   llvm::ConstantInt::get(i64Ty, BRONZE_ABI_UNDEFINED_BITS),
                                   "__bronze_module_env", 8);

    if (tables.fnSlotCount > 0) {
        llvm::ArrayType* ty = llvm::ArrayType::get(
            i64Ty, static_cast<uint64_t>(tables.fnSlotCount) * kFnSlotWords);
        tables.fnSlots = createTable(llvmModule, ty, llvm::ConstantAggregateZero::get(ty),
                                     "__bronze_fn_slots", 8);
    }
    return tables;
}

llvm::Value* icEntryPtr(llvm::IRBuilder<>& builder, llvm::GlobalVariable* icTable,
                        uint32_t icIndex) {
    return builder.CreateConstInBoundsGEP2_32(icTable->getValueType(), icTable, 0,
                                              icIndex * kIcEntryWords,
                                              "ic" + std::to_string(icIndex));
}

llvm::Value* emitKeyId(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                       uint32_t keyIndex) {
    if (keyIndex == BRONZE_ABI_FN_NAME_NONE || !tables.keyMap || keyIndex >= tables.keyCount) {
        return builder.getInt32(keyIndex);
    }
    llvm::Value* cell = builder.CreateConstInBoundsGEP2_32(tables.keyMap->getValueType(),
                                                           tables.keyMap, 0, keyIndex);
    return builder.CreateAlignedLoad(llvm::Type::getInt32Ty(builder.getContext()), cell,
                                     llvm::Align(4), "key" + std::to_string(keyIndex));
}

llvm::Value* globalCacheCellPtr(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                                uint32_t keyIndex) {
    if (!tables.globalCache || keyIndex >= tables.globalCacheSlotOf.size()) return nullptr;
    const uint32_t slot = tables.globalCacheSlotOf[keyIndex];
    if (slot == ModuleTables::kNoGlobalCacheSlot) return nullptr;
    return builder.CreateConstInBoundsGEP2_32(tables.globalCache->getValueType(),
                                              tables.globalCache, 0, slot,
                                              "gbl" + std::to_string(slot));
}

llvm::Value* fnSlotPtr(llvm::IRBuilder<>& builder, const ModuleTables& tables, uint32_t slot) {
    if (!tables.fnSlots || slot >= tables.fnSlotCount) return nullptr;
    return builder.CreateConstInBoundsGEP2_32(tables.fnSlots->getValueType(), tables.fnSlots, 0,
                                              slot * kFnSlotWords,
                                              "fnslot" + std::to_string(slot));
}

}  // namespace bronze::codegen_llvm

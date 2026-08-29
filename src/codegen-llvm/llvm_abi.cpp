#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_convert.h"

#include <cstdlib>
#include <cstring>
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
//
// The stride is a SITE — BRONZE_ABI_IC_WAYS entries — and way 0 sits at its
// front, so the pointer generated code passes the helpers is both "this site"
// and "this site's first entry" with no arithmetic between the two readings.
static_assert(BRONZE_ABI_IC_ENTRY_SIZE % sizeof(uint64_t) == 0,
              "the IC table is emitted as i64 words, so an entry must be a whole number of them");
static_assert(BRONZE_ABI_IC_SITE_SIZE == BRONZE_ABI_IC_ENTRY_SIZE * BRONZE_ABI_IC_WAYS,
              "a site is exactly its ways, with no cursor word: the install policy is "
              "move-to-front, which needs no state beyond the entries themselves");
static_assert(BRONZE_ABI_IC_SHAPE_OFFSET == 0, "word 0 of an entry is the cached shape");
static_assert(BRONZE_ABI_IC_SLOTWORD_OFFSET == 8, "word 1 of an entry is (depth << 32) | slot");
static_assert(BRONZE_ABI_IC_EPOCH_OFFSET == 16, "word 2 of an entry is the fill epoch");

// The fn-singleton table is i64 words for the same reason as the IC table: the
// helper takes `uint64_t*` and the two field offsets are ABI constants.
bool purePredicateHelpers() {
    static const bool enabled = [] {
        const char* env = std::getenv("BRONZE_NO_PURE_PREDICATES");
        return !(env != nullptr && std::strcmp(env, "1") == 0);
    }();
    return enabled;
}

static constexpr unsigned kFnSlotWords = BRONZE_ABI_FNSLOT_SIZE / sizeof(uint64_t);
static_assert(BRONZE_ABI_FNSLOT_SIZE % sizeof(uint64_t) == 0,
              "the fn-singleton table is emitted as i64 words, so an entry must be whole ones");
static_assert(BRONZE_ABI_FNSLOT_CODE_OFFSET == 0, "word 0 of a slot is the code pointer");
static_assert(BRONZE_ABI_FNSLOT_VALUE_OFFSET == 8, "word 1 of a slot is the Value");

void declareAbiSymbols(llvm::Module& llvmModule, llvm::LLVMContext& ctx, AbiFns& fns,
                       bool sharedRuntime) {
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
#define BRONZE_ABI_PU32   llvm::PointerType::getUnqual(ctx)
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

    // The import marking the header explains: without it, `&bronze_math_cos`
    // inside a loadable module is the address of that module's jump thunk, and
    // every code-pointer identity guard in generated code compares it against
    // the runtime's real address and loses. Applied to the whole registry
    // rather than to the handful of symbols a guard names today, because which
    // ones those are is a property of the fast paths and changes with them —
    // and because an indirect call through the IAT is what an imported call
    // costs anyway, thunk or no thunk.
#ifdef _WIN32
    if (sharedRuntime) {
#define BRONZE_ABI_LLVM_IMPORT(name, RET, PARAMS)         fns.name->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
        BRONZE_ABI_FUNCTIONS(BRONZE_ABI_LLVM_IMPORT)
#undef BRONZE_ABI_LLVM_IMPORT
    }
#else
    // ELF resolves an address-of through the GOT and Mach-O through its
    // equivalent, both to the ONE definition the dynamic linker chose, so the
    // guard already compares the right pointer there. Nothing to mark.
    (void)sharedRuntime;
#endif

    // The TLS-block accessor returns an address that is a per-thread
    // constant: readnone + willreturn is what lets LLVM fold a function's
    // fetch away when nothing uses it and CSE it when something does.
    fns.bronze_tls_block_addr->setDoesNotAccessMemory();
    fns.bronze_tls_block_addr->addFnAttr(llvm::Attribute::WillReturn);

    // The environment access-guard tripwire never returns (bronze_abi.h says
    // why), and saying so is what turns a guard's failure edge into an
    // `unreachable` with no merge behind it.
    fns.bronze_env_access_failed->addFnAttr(llvm::Attribute::NoReturn);
    fns.bronze_env_access_failed->addFnAttr(llvm::Attribute::Cold);

    // The two NUMBER-to-integer conversions are pure functions of one double —
    // rt_operator.cpp's toInt32 and toUint8Clamp are isfinite/trunc/fmod and
    // nothing else. Saying so matters more than the folding it buys: without
    // it, a call on the COLD arm of an inlined fast path is still a memory
    // clobber, so nothing loaded before the merge survives past it, and Stage
    // E1 measured exactly that — three ToInt32 calls per iteration suppressing
    // the optimization of a loop they are barely executed in.
    //
    // The boxed `bronze_to_int32` is deliberately NOT here: ToNumber runs
    // first, so a string is parsed and an object's valueOf is CALLED, which is
    // program text and can do anything.
    //
    // BRONZE_NO_PURE_CONVERSIONS=1 is the A/B seam. It is separate from
    // BRONZE_NO_INLINE_TOINT32 on purpose: the two mechanisms are worth
    // different things on different shapes and the 2x2 is what says so.
    if (pureConversionHelpers()) {
        for (llvm::Function* pure : {fns.bronze_to_int32_f64, fns.bronze_to_uint8_clamp_f64}) {
            pure->setDoesNotAccessMemory();
            pure->addFnAttr(llvm::Attribute::WillReturn);
            pure->addFnAttr(llvm::Attribute::Speculatable);
        }
    }

    // The PREDICATES, and the same lesson one step further out. Stage E2
    // priced `bronze_to_int32_f64`'s purity at 23.4 ns on a loop that barely
    // executed the call: what an opaque declaration costs is not the call, it
    // is that every value loaded before it has to be re-loaded after it.
    // `bronze_truthy` is the surviving instance of exactly that shape — the
    // cold arm of the inline truthiness split (llvm_ops.cpp) sits inside the
    // hot loop of `env_slot_kernel`, and because the declaration clobbers
    // memory the environment record's pointer, brand word and size word are
    // re-derived at the merge behind it, per iteration, on a path that is
    // never taken.
    //
    // The weakest attribute each one actually has, and no more:
    //
    //   - `bronze_truthy` (rt_convert.cpp) and `bronze_unbox_bool`, which IS
    //     it, READ the heap: a string's truthiness is its length and a
    //     BigInt's is its limbs, both behind a pointer. So `memory(read)`,
    //     not `memory(none)`. They write nothing, call no user code (7.1.2
    //     has no coercion row) and always return.
    //   - `bronze_is_nullish` inspects the tag bits and dereferences nothing,
    //     so it is `memory(none) speculatable` on the same terms as the two
    //     conversions above.
    //
    // NOT here, deliberately: `bronze_strict_eq` (it calls `recordHelperCall`,
    // which writes a counter), `bronze_typeof` (it can intern), and every
    // `bronze_rel_*` (an object operand's `valueOf` is program text).
    //
    // Soundness against the collector: the heap is a SEMISPACE COPYING
    // collector (runtime/heap.h), so a Value's raw bits are valid only between
    // collections — but none of the three helpers above allocates, so none of
    // them is a collection point, and `memory(read)` cannot be sunk past an
    // opaque call, which is what every collection point is, because such a call
    // may write what the read reads. Neither of the two ways a readonly call
    // may legally move can put it on the far side of a collection that moved
    // its operand.
    //
    // BRONZE_NO_PURE_PREDICATES=1 is the A/B seam.
    if (purePredicateHelpers()) {
        for (llvm::Function* ro : {fns.bronze_truthy, fns.bronze_unbox_bool}) {
            ro->setOnlyReadsMemory();
            ro->addFnAttr(llvm::Attribute::WillReturn);
        }
        fns.bronze_is_nullish->setDoesNotAccessMemory();
        fns.bronze_is_nullish->addFnAttr(llvm::Attribute::WillReturn);
        fns.bronze_is_nullish->addFnAttr(llvm::Attribute::Speculatable);
    }

#undef BRONZE_ABI_UNPAREN
#undef BRONZE_ABI_U64
#undef BRONZE_ABI_U32
#undef BRONZE_ABI_I32
#undef BRONZE_ABI_F64
#undef BRONZE_ABI_BOOL
#undef BRONZE_ABI_CSTR
#undef BRONZE_ABI_PU64
#undef BRONZE_ABI_PU32
#undef BRONZE_ABI_MU64
#undef BRONZE_ABI_TLSPTR
#undef BRONZE_ABI_FNPTR
#undef BRONZE_ABI_VOID
#undef BRONZE_ABI_NOARGS
}

AbiGlobals bindTlsBlock(llvm::IRBuilder<>& builder, const AbiFns& fns) {
    return bindTlsBlockAt(builder, builder.CreateCall(fns.bronze_tls_block_addr, {}, "tls"));
}

AbiGlobals bindTlsBlockAt(llvm::IRBuilder<>& builder, llvm::Value* base) {
    llvm::Type* i8Ty = builder.getInt8Ty();
    auto field = [&](uint64_t off, const char* name) -> llvm::Value* {
        if (off == 0) return base;
        return builder.CreateConstInBoundsGEP1_64(i8Ty, base, off, name);
    };
    AbiGlobals g;
    g.block_base = base;
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
    g.bronze_poly_ic_enabled = field(BRONZE_TLS_POLY_IC_ENABLED_OFF, "tls.poly_on");
    g.bronze_array_method_tbl = field(BRONZE_TLS_ARRAY_METHOD_TBL_OFF, "tls.arrtbl");
    g.bronze_method_call_ic_enabled =
        field(BRONZE_TLS_METHOD_CALL_IC_ENABLED_OFF, "tls.method_call_ic_on");
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
    tables.templateSlotCount = module.templateSiteCount;
    tables.globalCacheSlotOf = assignGlobalCacheSlots(module, tables.globalCacheCount);

    tables.staticSlotCount = module.staticSiteCount;
    if (tables.staticSlotCount > 0) {
        // Zero-initialized, unlike the caches above: zero is not a legal shape
        // address, so an unpublished cell simply never matches and the site
        // takes the path it would have taken without this mechanism at all.
        llvm::ArrayType* ty = llvm::ArrayType::get(i64Ty, tables.staticSlotCount);
        tables.staticSlots = createTable(llvmModule, ty, llvm::ConstantAggregateZero::get(ty),
                                         "__bronze_static_shapes", 8);
    }

    tables.slotReprFieldCount = static_cast<uint32_t>(module.slotReprFields.size());
    if (tables.slotReprFieldCount > 0) {
        tables.slotReprFields = createTable(
            llvmModule, llvm::ArrayType::get(i32Ty, module.slotReprFields.size()),
            llvm::ConstantDataArray::get(ctx, module.slotReprFields), "__bronze_repr_fields", 4);
        tables.slotReprFields->setConstant(true);
    }

    tables.familyClassCount = static_cast<uint32_t>(module.classFamilies.size());
    if (tables.familyClassCount > 0) {
        std::vector<uint32_t> classRows;
        std::vector<uint32_t> fieldRows;
        classRows.reserve(tables.familyClassCount * 2);
        for (const auto& cls : module.classFamilies) {
            classRows.push_back(static_cast<uint32_t>(fieldRows.size()));
            classRows.push_back(static_cast<uint32_t>(cls.fields.size()));
            for (const auto& f : cls.fields) {
                // Key index in the high bits, writability in bit 0: one word
                // per field keeps the table a single ConstantDataArray, and the
                // runtime needs exactly these two facts to check a shape's
                // prefix against the class.
                fieldRows.push_back((f.keyIndex << 1) | (f.writable ? 1u : 0u));
            }
        }
        // Zero fields across every class would make a table that matches every
        // shape; lowering does not emit such a class, and an empty field array
        // would be a global with no elements.
        if (!fieldRows.empty()) {
            tables.familyClasses = createTable(
                llvmModule, llvm::ArrayType::get(i32Ty, classRows.size()),
                llvm::ConstantDataArray::get(ctx, classRows), "__bronze_family_classes", 4);
            tables.familyClasses->setConstant(true);
            tables.familyFields = createTable(
                llvmModule, llvm::ArrayType::get(i32Ty, fieldRows.size()),
                llvm::ConstantDataArray::get(ctx, fieldRows), "__bronze_family_fields", 4);
            tables.familyFields->setConstant(true);
            // Zero until registration, and zero is below every id the runtime
            // hands out — so a guard that runs before init (there is none, but
            // the encoding should not depend on that) computes a huge unsigned
            // difference and misses.
            tables.familyBase = createTable(llvmModule, i64Ty, llvm::ConstantInt::get(i64Ty, 0),
                                            "__bronze_family_base", 8);
        } else {
            tables.familyClassCount = 0;
        }
    }

    if (module.icSiteCount > 0) {
        llvm::ArrayType* ty = llvm::ArrayType::get(
            i64Ty, static_cast<uint64_t>(module.icSiteCount) * kIcSiteWords);
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

    if (tables.templateSlotCount > 0) {
        // Undefined and not zero, for the global cache's reason: zero is the
        // double 0.0, and `template.cached` reads the cell and branches on
        // nullish. A zeroed table would report every site as already built.
        llvm::ArrayType* ty = llvm::ArrayType::get(i64Ty, tables.templateSlotCount);
        std::vector<uint64_t> undef(tables.templateSlotCount, BRONZE_ABI_UNDEFINED_BITS);
        tables.templateSlots = createTable(llvmModule, ty, llvm::ConstantDataArray::get(ctx, undef),
                                           "__bronze_template_slots", 8);
    }
    return tables;
}

llvm::Value* icEntryPtr(llvm::IRBuilder<>& builder, llvm::GlobalVariable* icTable,
                        uint32_t icIndex) {
    return builder.CreateConstInBoundsGEP2_32(icTable->getValueType(), icTable, 0,
                                              icIndex * kIcSiteWords,
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

llvm::Value* templateSlotPtr(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                             uint32_t site) {
    if (!tables.templateSlots || site >= tables.templateSlotCount) return nullptr;
    return builder.CreateConstInBoundsGEP2_32(tables.templateSlots->getValueType(),
                                              tables.templateSlots, 0, site,
                                              "tplslot" + std::to_string(site));
}

llvm::Value* fnSlotPtr(llvm::IRBuilder<>& builder, const ModuleTables& tables, uint32_t slot) {
    if (!tables.fnSlots || slot >= tables.fnSlotCount) return nullptr;
    return builder.CreateConstInBoundsGEP2_32(tables.fnSlots->getValueType(), tables.fnSlots, 0,
                                              slot * kFnSlotWords,
                                              "fnslot" + std::to_string(slot));
}

// The per-module TLS-block cache (seam: BRONZE_NO_TLS_CACHE=1 at CLI time).
//
// bindTlsBlock's prologue call is CSE'd within a function, but it is still a
// CALL — through the import table into the runtime DLL, into a dynamic-TLS
// address computation, and back — once per entry of every function that
// touches the block, with the full volatile-register clobber a call implies.
// The chunk-6 sampler charged the accessor itself 0.36 ms/frame on
// `many_meshes` and it was the top runtime line on `instanced`, all of it
// per-entry overhead: three.js's hot path is small functions entered millions
// of times a frame.
//
// The address the accessor answers is a per-thread CONSTANT (the runtime's
// own tls_block.h says so), so a module may remember it: a module-local
// `thread_local bronze_tls_block* cache`, null on each new thread, filled by
// the one accessor call the null-check miss path keeps. Module-local TLS is
// a TEB access — no call, no IAT, no clobber — and the null test is a
// perfectly predicted branch after the first entry on a thread.
//
// This runs as a REWRITE after emission rather than as a change to
// bindTlsBlock, because foldability is load-bearing: a function whose fetch
// is unused sees the plain call folded away by `readnone`, and an inline
// load-test-branch sequence would not fold (its miss-path store pins it). So
// only functions with a USED fetch are rewritten, and the entry-block call —
// which dominates every other fetch in the function, seam arms included —
// becomes the phi every fetch's users read. Entry-block allocas are hoisted
// into the new entry block the rewrite prepends, so mem2reg still sees every
// alloca in the function's entry block.
void cacheTlsFetches(llvm::Module& llvmModule, llvm::Function* tlsFn) {
    if (!tlsFn) return;
    llvm::LLVMContext& ctx = llvmModule.getContext();
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::GlobalVariable* cache = nullptr;

    for (llvm::Function& f : llvmModule) {
        if (f.isDeclaration()) continue;

        llvm::SmallVector<llvm::CallInst*, 8> calls;
        bool anyUsed = false;
        for (llvm::BasicBlock& bb : f) {
            for (llvm::Instruction& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call || call->getCalledOperand() != tlsFn) continue;
                calls.push_back(call);
                anyUsed = anyUsed || !call->use_empty();
            }
        }
        if (calls.empty() || !anyUsed) continue;

        llvm::BasicBlock& oldEntry = f.getEntryBlock();
        llvm::CallInst* entryCall = nullptr;
        for (llvm::CallInst* c : calls) {
            if (c->getParent() == &oldEntry) {
                entryCall = c;
                break;
            }
        }
        // No entry-block fetch means no fetch that dominates the others;
        // leave the function on the plain-call shape rather than reason about
        // dominance here. bindTlsBlock puts one in every function it touches,
        // so this is the wrapper-without-arguments case and nothing else.
        if (!entryCall) continue;

        if (!cache) {
            cache = new llvm::GlobalVariable(
                llvmModule, ptrTy, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantPointerNull::get(ptrTy), "__bronze_tls_block_cache",
                /*InsertBefore=*/nullptr, llvm::GlobalValue::GeneralDynamicTLSModel);
        }

        llvm::BasicBlock* checkBb =
            llvm::BasicBlock::Create(ctx, "tls.cache", &f, &oldEntry);
        llvm::BasicBlock* missBb =
            llvm::BasicBlock::Create(ctx, "tls.cache.miss", &f, &oldEntry);

        // Static allocas move first, so the new entry block holds them and
        // mem2reg's entry-block-only promotion still applies to all of them.
        llvm::SmallVector<llvm::AllocaInst*, 8> allocas;
        for (llvm::Instruction& inst : oldEntry) {
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(&inst)) allocas.push_back(a);
        }
        for (llvm::AllocaInst* a : allocas) a->moveBefore(*checkBb, checkBb->end());

        llvm::IRBuilder<> check(checkBb);
        llvm::LoadInst* cached =
            check.CreateAlignedLoad(ptrTy, cache, llvm::Align(8), "tls.cached");
        llvm::Value* isNull =
            check.CreateICmpEQ(cached, llvm::ConstantPointerNull::get(ptrTy), "tls.isnull");
        check.CreateCondBr(isNull, missBb, &oldEntry);

        llvm::IRBuilder<> miss(missBb);
        llvm::CallInst* fresh = miss.CreateCall(tlsFn, {}, "tls.fresh");
        miss.CreateAlignedStore(fresh, cache, llvm::Align(8));
        miss.CreateBr(&oldEntry);

        llvm::IRBuilder<> top(&oldEntry, oldEntry.begin());
        llvm::PHINode* tls = top.CreatePHI(ptrTy, 2, "tls.base");
        tls->addIncoming(cached, checkBb);
        tls->addIncoming(fresh, missBb);

        for (llvm::CallInst* c : calls) {
            c->replaceAllUsesWith(tls);
            c->eraseFromParent();
        }
    }
}

}  // namespace bronze::codegen_llvm

#pragma once

// The generated-code ABI expressed in LLVM terms. Every symbol compiled
// output links against is declared here, and only here, by rebinding the
// type tokens of the registry in src/abi/bronze_abi.h — so a signature
// drift between the C prototypes and the LLVM declarations is structurally
// impossible, which is the whole point of the registry (see that header for
// the sret-shift crash that made it a hard rule).
//
// The tables a generated object file owns on the ABI's behalf live here too —
// see ModuleTables below for why they are the module's data and not the
// runtime's.

#include <cstdint>
#include <vector>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "abi/bronze_abi.h"
#include "il/il.h"

namespace bronze::codegen_llvm {

// One llvm::Function* per entry in the ABI registry, named after the
// runtime symbol itself.
struct AbiFns {
#define BRONZE_ABI_FIELD(name, RET, PARAMS) llvm::Function* name;
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_FIELD)
#undef BRONZE_ABI_FIELD
};

// Likewise for the registry's data symbols.
struct AbiGlobals {
#define BRONZE_ABI_GLOBAL_FIELD(name, TYPE) llvm::GlobalVariable* name;
    BRONZE_ABI_GLOBALS(BRONZE_ABI_GLOBAL_FIELD)
#undef BRONZE_ABI_GLOBAL_FIELD
};

// Declares every registry symbol into `llvmModule`. Declarations only: the
// runtime owns every definition.
void declareAbiSymbols(llvm::Module& llvmModule, llvm::LLVMContext& ctx, AbiFns& fns,
                       AbiGlobals& globals);

// The four tables a compiled module owns, all internal to its object file.
//
// They are the module's data rather than the runtime's because a process can
// hold MORE THAN ONE compiled module, and every one of these is indexed by a
// number the compiler assigned. A runtime-owned vector indexed by
// module-assigned numbers has exactly one owner: a second module's site 7 is
// not the first module's site 7, and sharing the table would alias them.
// Module-owned is also the cheaper shape — every count is a compile-time fact,
// so a bounds check and a table-pointer load fall out of the fast paths and
// every cell is a constant address.
//
// The one thing that must NOT be module-local is the identity of a property
// name: two modules mentioning "position" have to mean one property. That is
// what `keyMap` buys. Each module still numbers its own keys 0..keyCount-1, so
// they stay immediates; at init the runtime INTERNS each string and the module
// records the process-wide id here. A helper argument is a load from this
// array, never the raw module number.
struct ModuleTables {
    // One BRONZE_ABI_IC_ENTRY_SIZE-byte entry per property site lowering
    // numbered. Null when the module has no property sites.
    llvm::GlobalVariable* icTable = nullptr;
    // [keyCount x i32]: module key index -> process-wide interned key id.
    llvm::GlobalVariable* keyMap = nullptr;
    // [globalCacheCount x i64]: one Value cell per distinct global a
    // `global.get` in this module names, undefined until the helper fills it.
    llvm::GlobalVariable* globalCache = nullptr;
    // [fnSlotCount x {i64 code, i64 value}]: one cache entry per IL function,
    // so every mention of one declaration shares one line.
    llvm::GlobalVariable* fnSlots = nullptr;
    // The module scope's environment record, as one Value cell. The top level
    // runs exactly once, so that scope has exactly one activation and its
    // record is a singleton — which is what lets a top-level function
    // declaration reach module-level `let`/`const` while staying a direct-call
    // target, instead of being handed the record through a calling convention
    // it does not have. `main` stores it before any statement runs; the module
    // functions that need it load it at entry.
    //
    // PER MODULE, and that is the whole point: as one runtime global it was the
    // record of whichever module initialized LAST, so module A's `bump()`
    // called after module B's init resolved its own `const registry` against
    // B's slots and read B's binding.
    llvm::GlobalVariable* moduleEnv = nullptr;

    uint32_t keyCount = 0;
    uint32_t globalCacheCount = 0;
    uint32_t fnSlotCount = 0;

    // Module key index -> index into `globalCache`, or kNoGlobalCacheSlot for a
    // key no `global.get` names. Dense over the key pool so the lookup is an
    // index, and assigned in IL order so the emitted table is deterministic.
    static constexpr uint32_t kNoGlobalCacheSlot = UINT32_MAX;
    std::vector<uint32_t> globalCacheSlotOf;
};

// Creates every table above into `llvmModule`, sized from the IL module.
//
// Zero-initialized where zero already means "cold", which is load-bearing
// twice over: a null `cached_shape` matches no real object (ObjectHeader::create
// refuses to build one without a shape), and a null fn-slot code word matches no
// mention. So a cold entry misses and falls into the helper, which fills it —
// there is no "is this entry valid" flag and none is needed.
//
// The two tables holding bare Values are the exceptions, and they start at
// `undefined` rather than zero, because zero is the double 0.0: a zeroed global
// cache would answer every global with 0 instead of missing into the helper.
ModuleTables createModuleTables(llvm::Module& llvmModule, llvm::LLVMContext& ctx,
                                const il::Module& module);

// Address of one entry in the IC table, as the `uint64_t*` the helpers take.
llvm::Value* icEntryPtr(llvm::IRBuilder<>& builder, llvm::GlobalVariable* icTable,
                        uint32_t icIndex);

// A key id in the form generated code must hand a helper: a load of
// `keyMap[keyIndex]`, emitted at the current insert point — which at every site
// that has one is the block already making the call, so no fast path pays for
// it.
//
// BRONZE_ABI_FN_NAME_NONE passes through unchanged: it is the ABSENCE of a
// name, not a key, and no module registers it. An index past the module's own
// key pool does too — it cannot name a string this module registered, and the
// helper that receives it raises the same diagnosed error it always did.
llvm::Value* emitKeyId(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                       uint32_t keyIndex);

// Address of the module's global-cache cell for `keyIndex`, as the `uint64_t*`
// `bronze_global_get` takes, or null when no cell was assigned.
llvm::Value* globalCacheCellPtr(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                                uint32_t keyIndex);

// Address of the module's fn-singleton entry for `slot`, as the `uint64_t*`
// `bronze_function_singleton` takes.
llvm::Value* fnSlotPtr(llvm::IRBuilder<>& builder, const ModuleTables& tables, uint32_t slot);

}  // namespace bronze::codegen_llvm

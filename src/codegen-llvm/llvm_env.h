#pragma once

// Captured-variable access in generated code. An environment record is a
// fixed layout the collector already scans generically (runtime/env.h), the
// depth and the slot index are compile-time constants, and the operation is a
// handful of loads — so a closure variable read in a loop costs loads, not a
// helper call per iteration. Every guard failure — wrong tag, wrong kind, a
// chain shorter than the depth, a slot past the record — falls through to the
// helper, which owns the fatal that names the lowering bug, and the TDZ hit
// falls through to bronze_env_get_tdz, which owns the ReferenceError.

#include <cstdint>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Whether this build drops the ACCESS guards — the object tag, the Env brand
// and the slot-range test that `emitEnvSlotPtr` re-derives at every access.
// Off unless BRONZE_ELIDE_ENV_GUARDS=1; llvm_env.cpp has what they are, what
// licenses dropping them, and the measurement that kept it a flag. The TDZ
// test is NOT one of them and is never dropped: it is 9.1.1.1.6, not a
// tripwire.
bool envAccessGuardsElided();

// Whether an access guard's FAILURE edge is a non-returning tripwire (the
// default) or the merging slow path it was before stage E2. Off — meaning the
// old merging shape — only under BRONZE_NO_ENV_TRIPWIRE=1, which exists so the
// two shapes can be timed against each other out of one binary. This is not a
// semantics switch in either position: both keep every guard armed and both
// end a failure in the same fatal.
bool envTripwireEdges();

// Emits an environment slot read and returns its i64 (NaN-boxed) result.
// With `tdz` set, a slot still holding the uninitialized marker takes the
// helper path, which raises the ReferenceError `keyIndex` names.
llvm::Value* emitEnvGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                        const ModuleTables& tables, llvm::Value* envBits, uint32_t depth,
                        uint32_t index, bool tdz, uint32_t keyIndex, bool elideGuards);

// Emits an environment slot write.
void emitEnvSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* envBits,
                uint32_t depth, uint32_t index, llvm::Value* valBits, bool elideGuards);

// The record `hops` parent links up from `envBits`, as a Value. `hops == 0` is
// `envBits` itself and emits nothing.
//
// This is the chain walk `emitEnvSlotPtr` performs, without the slot read at
// the end and without the access guards — deliberately, and on the narrowest of
// the three licences. The guards are TRIPWIRES for a lowering bug (llvm_env.cpp
// says so at length), and the one caller of this is a DIRECT CALL to a closure,
// where a wrong record is not a wrong load but a wrong function's environment.
// The only thing that could make it wrong is the hop count, which no guard here
// could check: an Env record's brand does not say which scope it is. So the
// count is verified where it is made — the scope plan (lower_scope.cpp
// `planStableFunctionSlots` and `recordStableFunctionSlot`, which refuses any
// binding not held by the record the closure was created over) — and the walk
// itself is the loads.
//
// Each parent load is `!invariant.load`: a record's parent link is written once,
// at creation, and there is no operation in the language that rewrites one.
llvm::Value* emitEnvAncestor(llvm::IRBuilder<>& builder, llvm::Value* envBits, uint32_t hops);

}  // namespace bronze::codegen_llvm

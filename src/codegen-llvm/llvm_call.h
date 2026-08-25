#pragma once

#include <cstdint>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits an inlined DynamicCall fast path in generated code:
// Guard:
//   - Callee is an Object-tagged value (`(callee >> 48) == TAG_OBJECT`)
//   - Callee carries HeapKind::Function flags (`BRONZE_ABI_OBJ_FLAGS_FUNCTION`)
//   - Function arity is <= call site argc (`fn->arity <= argc`)
//   - Feature is enabled (the TLS block's `inline_call_enabled != 0`)
// Fast path:
//   Loads `fn->env_record` and `fn->code`, and invokes `code(env, thisVal, argc, argv)`
//   directly in generated LLVM IR via indirect function pointer call.
// Fallback:
//   Calls `bronze_dynamic_call(callee, thisVal, argc, argv)` on any miss.
llvm::Value* emitDynamicCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                   const AbiGlobals& globals, llvm::Value* callee,
                                   llvm::Value* thisVal, uint32_t argc, llvm::Value* argv,
                                   llvm::Function* knownWrapper = nullptr);

// Emits an inlined direct dispatch for Array.prototype.push:
// Guard:
//   - `this` is an Array object with no side properties
//   - `callee` is a function with code pointer == `bronze_array_push`
//   - Array has headroom in its element backing block (`head_offset + length < capacity`)
// Fast path:
//   Stores `argVal` into `elements[head_offset + length]`, increments `length`, and returns new length as boxed double.
// Fallback:
//   Calls `bronze_dynamic_call(callee, thisVal, argc, argv)` on any miss.
llvm::Value* emitArrayPushDirectCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                     llvm::Value* calleeBits, llvm::Value* thisBits,
                                     uint32_t argc, llvm::Value* argvPtr,
                                     llvm::Value* argVal);

// The two conversions a TYPED CALLING CONVENTION needs at its boundaries, and
// the reason they are inline rather than the ABI helpers they defer to.
//
// A typed slot is what `--pins param` / `return` and inference's proven
// signatures buy. Every caller that cannot use the typed entry still enters
// through the uniform wrapper, and every boxed consumer of a typed result still
// wants a Value — so the conversions run on the SAME hot paths the typed slot
// exists to make fast. `bronze_unbox_f64` is a cross-module call to ToNumber;
// paying one per argument would have made a typed parameter a regression.
//
// bronze's NaN box puts every Number at or below NUMBER_MAX_BITS, so the
// common case is one unsigned compare and a bitcast either way, and the miss
// arm is exactly the helper. Semantics are the helper's, unchanged.

// The metadata a direct method-call edge tags its call with, and the pass that
// spends it.
//
// A direct edge deletes the uniform boundary — no argument vector, no wrapper —
// and that is worth about half a nanosecond. What the boundary actually COSTS
// is on the callee's side and no call site can delete it: on Windows x64 a
// float-heavy method spills ten callee-saved XMM registers at entry and reloads
// them at exit, pushes and pops a GC root frame, fetches its thread's ABI
// block, and re-derives every field it reads through a guard the caller's loop
// already established. Twenty vector stores and their loads are most of the
// twelve nanoseconds `Matrix4.multiplyMatrices` pays per call.
//
// Only INLINING removes those, and LLVM's cost model refuses a body this size
// at an ordinary call site. So the edge asks for it — at the SITE, not on the
// function, so the boxed path keeps calling one out-of-line copy — and asks
// only where the body is small enough that the ask is not code-size vandalism.
// The budget is a count of the callee's IR instructions, which is why this is a
// pass over the finished module rather than a decision at emission: a callee is
// commonly emitted after its caller.
inline constexpr const char* kDirectMethodMD = "bronze.direct_method";

// Marks every direct method-call site whose callee fits the budget
// `alwaysinline`. Runs after emission and before optimization.
void markDirectMethodInlining(llvm::Module& llvmModule);

// ToNumber (7.1.4) of a boxed value, as a double.
llvm::Value* emitToNumberInline(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* bits);

// A double as a Value. The canonical-NaN normalization `Value::fromDouble`
// performs, branchless — a select, because a NaN is rare and a branch on it
// would be a mispredict rather than a saving.
llvm::Value* emitBoxF64Inline(llvm::IRBuilder<>& builder, llvm::Value* value);

}  // namespace bronze::codegen_llvm

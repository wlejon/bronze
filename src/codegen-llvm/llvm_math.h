#pragma once

// Direct dispatch of a dynamic call whose callee was read as `sqrt`, `sin`,
// `cos`, `abs`, `min` or `max` — the Math members generated code can guard by
// CODE POINTER (bronze_abi.h's bronze_math_* symbols) and compute without
// bronze_dynamic_call's trampoline. The guard is the whole soundness story:
// a program that overwrote `Math.sqrt`, a callee that is not the intrinsic at
// all, or a non-number argument all miss the pointer/tag compares and take
// the ordinary call, so the fast path never assumes what lowering could not
// prove.
//
// Determinism: sqrt and abs are inlined as llvm.sqrt.f64 / llvm.fabs.f64,
// which are IEEE-exact; sin, cos, min and max call the exact scalar kernels
// the helper path itself runs (builtin_math.cpp), so the two paths are the
// same instructions. No fast-math flags anywhere.

#include <cstdint>
#include <optional>
#include <string_view>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

enum class MathIntrinsic { Sqrt, Sin, Cos, Abs, Min, Max, Imul, Floor, Ceil, Round };

// The intrinsic a call site may dispatch directly, decided from the key its
// callee was read by and the site's compile-time argc — the unary four take
// exactly one argument, min/max/imul exactly two (any other argc keeps the plain
// call, whose variadic semantics the helper owns).
std::optional<MathIntrinsic> mathIntrinsicFor(std::string_view keyStr, uint32_t argc);

// Emits the guarded call: the fast arm computes inline, the slow arm is the
// fallback invoking `missEmit()`. Returns the i64 (NaN-boxed) or double result.
// `args` are the site's argument values (1 or 2 of them, matching the intrinsic).
// `lastGuardedMathFn` caches the callee guard across consecutive calls.
// If `resultAsF64` is true, returns raw double instead of NaN-boxed i64.
llvm::Value* emitMathDirectCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                MathIntrinsic kind, llvm::Value* calleeBits,
                                llvm::Value* thisBits, uint32_t argc,
                                llvm::ArrayRef<llvm::Value*> args,
                                llvm::function_ref<llvm::Value*()> missEmit,
                                llvm::Value** lastGuardedMathFn = nullptr,
                                bool resultAsF64 = false);

// Computes the raw double result of a MathIntrinsic inline on validated number arguments.
llvm::Value* emitMathComputeRaw(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                MathIntrinsic kind, llvm::ArrayRef<llvm::Value*> args);

// Computes the result of a MathIntrinsic inline on validated number arguments.
llvm::Value* emitMathCompute(llvm::IRBuilder<>& builder, const AbiFns& abi,
                             MathIntrinsic kind, llvm::ArrayRef<llvm::Value*> args);

// Direct method-call fast path for Math intrinsics called as `Math.<fn>(...)`.
// Checks receiver object/shape and intrinsic code pointer. If all match and
// arguments are numbers, computes the result inline without allocating argv.
// Miss fallback invokes `missEmit` to build argv and call emitMethodCallInline.
llvm::Value* emitMethodCallMathDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, MathIntrinsic kind, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex, uint32_t argc,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::function_ref<llvm::Value*()> missEmit,
    llvm::Value** lastGuardedMathRecv = nullptr,
    bool resultAsF64 = false);

}  // namespace bronze::codegen_llvm

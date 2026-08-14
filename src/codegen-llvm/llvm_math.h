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
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

enum class MathIntrinsic { Sqrt, Sin, Cos, Abs, Min, Max };

// The intrinsic a call site may dispatch directly, decided from the key its
// callee was read by and the site's compile-time argc — the unary four take
// exactly one argument, min/max exactly two (any other argc keeps the plain
// call, whose variadic semantics the helper owns).
std::optional<MathIntrinsic> mathIntrinsicFor(std::string_view keyStr, uint32_t argc);

// Emits the guarded call: the fast arm computes inline, the slow arm is the
// ordinary bronze_dynamic_call(callee, this, argc, argv). Returns the i64
// (NaN-boxed) result. `args` are the site's argument values (1 or 2 of them,
// matching the intrinsic); `argvPtr` is the already-filled root-frame argv
// block the slow arm passes through.
llvm::Value* emitMathDirectCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                MathIntrinsic kind, llvm::Value* calleeBits,
                                llvm::Value* thisBits, uint32_t argc, llvm::Value* argvPtr,
                                llvm::ArrayRef<llvm::Value*> args);

}  // namespace bronze::codegen_llvm

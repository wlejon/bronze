#pragma once

// ECMA-262 7.1.6 ToInt32 on a value already known to be a Number, emitted
// INLINE. Every bitwise operator, every shift, and every integer typed-array
// store is one of these, so on a kernel that does three bitwise ops per
// iteration the helper call was measured (Stage E1) as more than half the
// whole loop — not only for the call itself, but because an opaque external
// call is a barrier the surrounding code cannot be optimized across.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Whether this build emits the inline fast path. On unless
// BRONZE_NO_INLINE_TOINT32=1, which is the A/B seam: it makes every site
// below a plain call to `bronze_to_int32_f64` and leaves the rest of the
// compiler alone, so both columns come out of one binary.
bool toInt32InlineEnabled();

// ToInt32 of the double `dbl`, as an i32.
//
// THE FAST PATH AND WHY IT IS EXACT. ToInt32 truncates toward zero, reduces
// modulo 2^32 and reinterprets the residue as signed. For any double whose
// truncation fits in an int64 — which is every double in (-2^63, 2^63) plus
// -2^63 itself — `fptosi ... to i64` IS the mathematical truncation, exactly,
// and `trunc i64 to i32` IS the modulo-2^32 reduction with the signed
// reinterpretation. So the whole conversion is two machine operations behind
// one range test, and the test is what keeps `fptosi` out of the poison it
// produces for NaN, the infinities and anything past int64.
//
// The range test is the pair of ORDERED compares `dbl >= -2^63` and
// `dbl < 2^63`: NaN answers false to both, so it leaves through the same edge
// as the infinities. Both bounds are exact doubles (powers of two), and the
// upper is strict because 2^63 does not fit; the largest double below it,
// 2^63 - 1024, does.
//
// Everything the fast path declines — NaN, +-Inf, |x| >= 2^63 — goes to
// `bronze_to_int32_f64`, which is the same fmod-based reduction it always was.
// -0 needs no special case: `fptosi` of -0.0 is 0, and ToInt32(-0) is +0.
//
// The builder is left in a fresh block (the merge), the way emitEnvGet and the
// ToNumeric fast path leave it.
llvm::Value* emitToInt32F64(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* dbl);

}  // namespace bronze::codegen_llvm

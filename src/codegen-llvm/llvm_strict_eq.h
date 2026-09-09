#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// The seam word for the inline `===`. `bronze_tls_block_addr` is `readnone` +
// `willreturn`, so this call CSEs with the prologue's fetch and a loop hoists
// it — the same shape llvm_iter.cpp's `emitIterFastEnabled` uses, and for the
// same reason: this file has `AbiFns` but not `AbiGlobals`.
llvm::Value* emitStrictEqInlineEnabled(llvm::IRBuilder<>& builder, const AbiFns& abi);

// `a === b` over boxed operands, as three arms and a helper.
//
// The helper (rt_convert.cpp `bronze_strict_eq`) is four tests, and the
// chunk-4 sampler charged the CALL to it 2.71 % of the `many_meshes` frame —
// three.js asks this question about markers, `undefined`, `null` and object
// identity thousands of times a draw. Every one of those is answered here.
//
// The arms, and why each is exactly the helper's answer:
//
//  1. BOTH NUMBERS -> one ORDERED fcmp. This arm exists first and not as a
//     special case of bit equality, because bit equality gets both of the
//     IEEE-754 edges wrong in opposite directions: two values that are the
//     SAME NaN have identical bits and `===` says false, and `+0` and `-0`
//     have different bits and `===` says true. `fcmp oeq` is both of those,
//     which is why the helper's number row is `==` on doubles and not on bits.
//
//  2. NOT both numbers, and the BITS ARE EQUAL -> true. Sound because equal
//     bits means both operands are numbers or neither is, and arm 1 already
//     took the case where both are: so here neither is a number, and identical
//     bits are the same object, the same string, the same BigInt, the same
//     symbol or the same immediate. Every one of those is `===`.
//
//  3. Bits differ -> false, UNLESS the left operand is a String or a BigInt.
//     Those are the only two rows in the helper that can answer true for
//     different bits (content equality and mathematical-value equality); for
//     every other tag "different bits" is the helper's own final `aBits ==
//     bBits`. A number's top sixteen bits are below every tag, so the tag test
//     is safe on an operand arm 1 rejected.
//
// A String or BigInt on the left is the only thing that reaches the helper,
// and there it takes the path it always took.
llvm::Value* emitStrictEq(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* lhs,
                          llvm::Value* rhs);

}  // namespace bronze::codegen_llvm

#pragma once

// THE WRITE BARRIERS FOR `--pins` (src/types/pins.h, stage B1).
//
// A pin is a promise the invocation makes about the program, and until this
// stage nothing held the program to it: a manifest that named a field the
// program sometimes wrote a string into compiled to a raw unbox of a string
// pointer, and the result was undefined behaviour with no diagnostic anywhere.
// These emitters make that case DEFINED — a catchable TypeError naming the
// manifest line — without touching the read side, which is where the whole
// performance model lives.
//
// WHERE A BARRIER GOES, AND WHY NOT AT THE READ. A pinned read spends the
// claim unconditionally; that IS the pin. Checking there would restore exactly
// the guard the manifest was written to remove. So the claim is checked where
// it can be CONTRADICTED — the store, the enumerated call site, and the boxed
// wrapper — and a store the compiler has already proved emits nothing at all.
//
// WHAT THE TESTS COST. `Number` and `NumberOrNullish` are unsigned compares
// against constants on a value already in a register: the whole barrier is one
// compare and a cold branch, and it disappears entirely wherever lowering
// hands over an f64. `DenseArray` is a call, because "is this a plain JS
// array" is a heap question with no inline form — it is emitted only at a
// store to a `numeric-elements` FIELD, which is a constructor-shaped site and
// not a loop-shaped one.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"
#include "il/il.h"

namespace bronze::codegen_llvm {

// `Op::PinGuard`: test `bits` against `kind` and raise a TypeError naming the
// manifest line `keyIndex` when it fails.
//
// The violating arm RAISES AND LEAVES, to `unwind` — the block the pending-cell
// test after any throwing instruction would have branched to, so a violation
// inside a `try` still reaches that handler and one outside it still pops the
// root frame and returns. It does not merge back, and that is what makes the
// barrier free where it is kept: the guard's kept edge is the ONLY edge that
// falls out of it, so nothing after it stands at a join, `il::canCollect` and
// `il::canThrow` both answer no for this form, and every receiver proof, every
// cached array header and every unreloaded root slot survives the guard.
//
// It was not always so. When the violating arm merged back and leaned on the
// exception check to skip the store, a `numeric-elements` manifest put a
// possible collection between every element read of `Matrix4.copy` and the
// next: no run of reads survived one, so all sixteen took the property ladder
// the unpinned build proved away in one, and each of the sixteen raw stores
// re-derived the element base from a reloaded receiver. The pin made that
// kernel 2.4x SLOWER than no pin at all.
//
// Leaves the builder in the kept block.
//
// `unwind` is ignored by the `DenseArray` form, whose whole test is a call: it
// returns on both outcomes, so `il::canThrow` still puts the ordinary check
// after it.
void emitPinGuard(llvm::IRBuilder<>& builder, const AbiFns& abi, const ModuleTables& tables,
                  llvm::Value* bits, uint32_t keyIndex, il::PinBarrier kind,
                  llvm::BasicBlock* unwind);

// The boxed wrapper's form of the same claim for a `param <owner>(<p>): number`
// entry: check `bits`, and hand back the double.
//
// It REPLACES `emitToNumberInline` at a pinned position rather than joining
// it, which is why the barrier is not a cost here but a saving: the uniform
// path used to run ToNumber (7.1.4) on every argument reaching a pinned
// parameter, so `f("5")` saw 5 and the manifest's promise was quietly made
// true by coercion. A pin says the caller passes a Number; a caller that does
// not is now told so.
//
// On the violating edge the wrapper RETURNS `undefined` immediately — the
// pending cell carries the exception and the caller's own check picks it up —
// so this must only be used in a function whose return type is the uniform
// i64. Leaves the builder in the block where the argument is good.
llvm::Value* emitPinnedParamUnbox(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                  const ModuleTables& tables, llvm::Value* bits,
                                  uint32_t keyIndex);

// The DIRECT METHOD EDGE's form of the same check (stage 3.3's typed entry,
// llvm_ops_call.cpp). It is a third shape and not a repeat, because the site
// sits inside an arbitrary function that cannot simply return: the violating
// edge raises and joins the call's own merge block with `undefined`, which is
// the value that site would have produced anyway with an exception pending.
//
// `incoming` collects (value, block) pairs for `joinBb`'s result phi, so the
// caller adds them beside the hit and miss edges it already has. Leaves the
// builder in the block where the argument is good.
llvm::Value* emitPinnedArgUnbox(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                const ModuleTables& tables, llvm::Value* bits, uint32_t keyIndex,
                                llvm::BasicBlock* joinBb,
                                std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>>& incoming);

}  // namespace bronze::codegen_llvm

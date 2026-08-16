#pragma once

// What a property READ's inline cache and a property WRITE's inline cache
// emit identically: the constant-key index test that opens both fast paths,
// the walk up a prototype chain to the depth the cached entry recorded, and
// the load of a slot off whichever object that walk landed on.
//
// They are here rather than in either emitter because a guard that differed
// between the two would be a cache that hits on the read and misses on the
// write for one and the same shape — the drift an inline cache cannot
// survive, and the reason the two emitters may live in separate files at all.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace bronze::codegen_llvm {

// A load whose address holds a value that cannot change for the object's
// lifetime, told to LLVM so a second read of it folds into the first.
void markInvariant(llvm::LoadInst* load, llvm::LLVMContext& ctx);

// Is this compile-time key string a canonical array index (6.1.7)? Leading
// zeros and anything past 2^32-2 are not, so `a["01"]` and `a["4294967295"]`
// take the named-property path both the read and the write send them down.
std::optional<uint32_t> parseIndexKey(std::string_view key);

struct ProtoWalkResult {
    llvm::Value* holderHdr{nullptr};
    llvm::BasicBlock* latchBb{nullptr};
};

// Walks `depth` steps along the prototype chain starting from `startShape`.
// If any check fails (null root, non-object proto, non-plain proto, dictionary
// shape), branches to slowBb. When `depth` steps are traversed, branches to
// `successBb`.
ProtoWalkResult emitProtoChainWalk(llvm::IRBuilder<>& builder, llvm::LLVMContext& ctx,
                                   llvm::Function* fn, llvm::Value* startShape,
                                   llvm::Value* depth, llvm::BasicBlock* entryBb,
                                   llvm::BasicBlock* slowBb, llvm::BasicBlock* successBb,
                                   const std::string& prefix);

// Loads a slot value from holderHdr at slot32 (inline if < 4, overflow if >= 4).
// If overflow is required and not present/not an object, branches to slowBb.
// On success, branches to `successBb` and returns the loaded Value bits via PHI.
llvm::Value* emitObjectSlotLoad(llvm::IRBuilder<>& builder, llvm::LLVMContext& ctx,
                                llvm::Function* fn, llvm::Value* holderHdr, llvm::Value* slot32,
                                llvm::BasicBlock* slowBb, llvm::BasicBlock* successBb,
                                const std::string& prefix);

}  // namespace bronze::codegen_llvm

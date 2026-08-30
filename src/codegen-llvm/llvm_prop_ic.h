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

// `il::parseIndexKey` (il/key.h) is what answers "is this key a canonical array
// index": it moved into `il` when the guarded-region pass began asking the same
// question, because the two passes have to give the same answer exactly.

struct IcWayScanResult {
    // The matched way's entry pointer, as a PHI over the ways that could have
    // matched. Every field the rest of the fast path reads — the slot word, the
    // fill epoch — is read off THIS and not off the site.
    llvm::Value* entry{nullptr};
    // The receiver's shape, loaded once inside the scan and live from there on.
    llvm::Value* shape{nullptr};
    // Where control is when the scan has matched; the builder is left here.
    llvm::BasicBlock* hitBb{nullptr};
};

// The way scan that opens a property READ's fast path: is the receiver plain,
// and does any of this site's ways name its shape?
//
// Way 0 is compared first and unconditionally, so a monomorphic site pays
// exactly the load and compare it paid when a site WAS one entry. Ways
// 1..N-1 are reached only after way 0 misses, and only through one load of
// `polyEnabledField` — which is what makes BRONZE_NO_POLY_IC=1 a real A/B
// rather than a slower road to the same answer, since a way installed before
// the flag came down would otherwise still be found.
//
// The plain check gates the whole scan rather than being AND'd into each way:
// the shape word of an array or a function header is a different field
// entirely, and comparing it against a cached Shape* is a match this cache
// must never be able to make.
//
// `notPlainBb` is where a NON-plain receiver goes; the default is `slowBb`,
// which is what every caller wanted while the plain object was the only kind
// with a shape. A caller that can answer for another kind — the read's
// function-statics arm — passes its own block instead, and gets the plain
// path's instruction sequence unchanged in exchange: the extension hangs off
// an edge that was already a branch to the helper, so nothing is added in
// front of the hit every other receiver takes.
IcWayScanResult emitIcWayScan(llvm::IRBuilder<>& builder, llvm::LLVMContext& ctx,
                              llvm::Function* fn, llvm::Value* site, llvm::Value* hdr,
                              llvm::Value* flags, llvm::Value* polyEnabledField,
                              llvm::BasicBlock* slowBb, const std::string& prefix,
                              llvm::BasicBlock* notPlainBb = nullptr);

struct ProtoWalkResult {
    llvm::Value* holderHdr{nullptr};
    llvm::BasicBlock* latchBb{nullptr};
};

// THE SEAM for the function-statics arm of a property read
// (`BRONZE_NO_FN_STATICS_IC=1`), read once and cached. With it down, a read
// whose receiver is a FUNCTION goes straight to the helper the way it did
// before the arm existed. The runtime reads the SAME variable
// (rt_property.h `fnStaticsIcReadSeam`) and stops warming entries with it, so
// one binary A/Bs the whole mechanism rather than half of it.
bool fnStaticsIcDisabled();

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

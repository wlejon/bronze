#pragma once

// Property reads and writes in generated code, including the inlined
// inline-cache check generated code performs itself.

#include <cstdint>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "codegen-llvm/llvm_static_slot.h"

#include <string_view>

namespace bronze::codegen_llvm {

// Emits a property read and returns its i64 (NaN-boxed) result.
//
// `monomorphic` is an identity proof about the receiver. It reaches the IL and
// the stats; the emitted sequence does not branch on it, because the sequence
// IS an inline cache and a cache is what an unproven site wants too.
//
// `site` is the stronger claim — a proven class layout naming a constant
// instance slot — and it DOES change what is emitted: a compare-and-constant-
// load fast path in front of the cache, guarding either on the one shape the
// site pinned or on the receiver's whole `extends` subtree. See
// llvm_static_slot.h. `objSlot` is the receiver's GC root slot address, which the
// one-shot publish behind the fallback call reloads the receiver from — see
// emitStaticSlotPublish; null disables the publish rather than reading a
// register the collector may have invalidated.
// `keyStr` (when non-empty) enables compile-time index or length fast paths.
// `globals` carries the prototype-mutation epoch, which the depth > 0
// proto-hit path re-checks exactly as InlineCache::describes does.
// `proof`, when live, is a receiver already proven a dense array long enough
// for this index (llvm_recv_proof.h): the cache then emits a four-instruction
// arm in front of the whole ladder below, and updates the proof in place with
// the version that reaches its foot. Null, or not live, changes nothing.
llvm::Value* emitPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                         const AbiGlobals& globals, const ModuleTables& tables,
                         llvm::Value* objBits, llvm::Value* objSlot, uint32_t keyIndex,
                         uint32_t icIndex, bool monomorphic, const StaticSite& site,
                         std::string_view keyStr = {}, ReceiverProof* proof = nullptr);

// Property writes. The inline paths are the own-slot hit and the
// shape-transition hit (a constructor body's repeated property add); every
// guard miss falls back to bronze_prop_set.
void emitPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
                 const ModuleTables& tables, llvm::Value* objBits, llvm::Value* objSlot,
                 uint32_t keyIndex, llvm::Value* valBits, uint32_t icIndex, bool strict,
                 bool monomorphic, const StaticSite& site, ValueRepr valRepr,
                 std::string_view keyStr = {});

}  // namespace bronze::codegen_llvm

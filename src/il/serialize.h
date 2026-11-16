#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "il/il.h"

// The IL as bytes, for the on-disk code cache (src/eval/code_cache.h).
//
// A binary encoding of `Module` that decodes back to a module equal to the one
// encoded, field for field — except the per-file source TEXTS and line tables,
// which are never written: the cache re-reads every source file to validate it
// anyway, so a cached module carries only the file names and the reader rebuilds
// both from the texts it was handed (`attachSources`).
//
// The decoder is bounds-checked throughout and fails with a message rather than
// reading past its input: a truncated or corrupt file is a cache miss, never a
// crash. It is not a validator of the IL's own invariants — the cache checksums
// what it wrote, and a checksum-valid payload is one this encoder produced.
//
// Adding a field to `Instruction`, `Block`, `Function` or `Module` means adding
// it here and bumping `kFormatVersion`; the layout guards in serialize.cpp stop
// a build that forgets the first half.
namespace bronze::il {

// 12: `object.proto`, and prop.set / elem.set carry kWriteDefine (il.h).
// 13: prop.get / prop.set carry staticSlotGuessed (il.h).
// 14: prop.get carries icPolymorphic (il.h).
// 16: is.shape, and prop.get/prop.set carry shapeGuarded (il.h).
// 17: feedback.record (il_op.h).
// 18: numberHint (il.h), from a type-feedback profile.
// 19: Block::cold (il.h), a profile-guided region's slow copy.
// 20: truthHint (il.h), from a type-feedback profile.
// 21: icChain (il.h), from a type-feedback profile.
// 23: create.object's immI32 and fnFlags bits 8..12 carry an inline-slot
//     prediction (BRONZE_ABI_FN_INLINE_SLOTS_SHIFT); slots split at 16.
// 24: is.array (il_op.h).
// 26: Module::icLayoutDigest, feedbackApplied, harvestOutPath (the IC harvest).
// 28: Module::hotFunctions, coldFunctions (lower/function_placement.h).
// 30: Module::layoutCells (opt/spec/feedback_plan.h).
// 31: a layout cell's member carries `number` (il.h).
// 32: object members and family fields, Module::slotReprObjectFields.
// 33: Instruction::slowSite, Module::slowSiteCount (il/feedback_site.h).
// 34: prop.get no longer carries icAbsent.
// 35: nor icPolymorphic and icChain.
// 36: nor icMonomorphic.
// 37: the optimiser's ops and claims are gone: no static-slot, family,
//     transition-run, define-add, typed-target, inline-frame or hint fields on
//     an instruction, no copy class on a block, no typed origin or inline
//     frames on a function, no class families, layout cells, slot-repr lists,
//     feedbackApplied or function placement on a module.
// 38: the speculation ops (il_op.h, resume.point .. is.hole), Type::Ptr,
//     Function::speculatedFrom and Module::shapeCells.
// 39: resume.call (il_op.h) and Instruction::resumeAfter.
// 42: member.cell / member.publish and Module::memberCells.
// 43: shape.in.
// 44: absent member cells (Module::MemberKind::Absent).
inline constexpr uint32_t kFormatVersion = 44;

std::string serializeModule(const Module& module);

// False with `err` set on any malformed input; `out` is then unspecified.
bool deserializeModule(std::string_view bytes, Module& out, std::string& err);

// Fills `sourceTexts` (when `retainTexts`) and `lineTables` from the program's
// file texts, in `sourceFiles` order — what lowering built them from.
void attachSources(Module& module, const std::vector<std::string_view>& texts, bool retainTexts);

}  // namespace bronze::il

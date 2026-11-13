#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "il/il.h"
#include "lower/guard_region.h"

namespace bronze::lower {

// The instructions the guarded-region rewrite makes, shared by its files
// (guard_region_build.cpp, guard_region_reenter.cpp).

inline il::Instruction makeIsNumber(il::ValueId result, il::ValueId operand) {
    il::Instruction inst;
    inst.op = il::Op::IsNumber;
    inst.type = il::Type::Bool;
    inst.result = result;
    inst.operands = {operand};
    return inst;
}

inline il::Instruction makeIsDenseArray(il::ValueId result, il::ValueId operand, uint32_t maxIndex) {
    il::Instruction inst;
    inst.op = il::Op::IsDenseArray;
    inst.type = il::Type::Bool;
    inst.result = result;
    inst.operands = {operand};
    inst.immI32 = static_cast<int32_t>(maxIndex);
    return inst;
}

inline il::Instruction makeIsNumberArray(il::ValueId result, il::ValueId operand, uint32_t maxIndex) {
    il::Instruction inst = makeIsDenseArray(result, operand, maxIndex);
    inst.op = il::Op::IsNumberArray;
    return inst;
}

inline il::Instruction makeIsShape(il::ValueId result, il::ValueId operand, const RegionPlan::ShapeTest& test) {
    il::Instruction inst;
    inst.op = il::Op::IsShape;
    inst.type = il::Type::Bool;
    inst.result = result;
    inst.operands = {operand};
    inst.staticSlot = test.slot;
    inst.staticCellIndex = test.cell;
    inst.familyLo = test.familyLo;
    inst.familySpan = test.familySpan;
    return inst;
}

inline il::Instruction makeRawUnbox(il::ValueId result, il::ValueId operand) {
    il::Instruction inst;
    inst.op = il::Op::Unbox;
    inst.type = il::Type::F64;
    inst.result = result;
    inst.operands = {operand};
    inst.rawUnbox = true;
    return inst;
}

inline il::Instruction makeBoxF64(il::ValueId result, il::ValueId operand) {
    il::Instruction inst;
    inst.op = il::Op::Box;
    inst.type = il::Type::Dynamic;
    inst.boxType = il::Type::F64;
    inst.result = result;
    inst.operands = {operand};
    return inst;
}

// The boxed form of a tested Number, rebuilt from its double (il.h `rawBox`).
inline il::Instruction makeRawBoxF64(il::ValueId result, il::ValueId operand) {
    il::Instruction inst = makeBoxF64(result, operand);
    inst.rawBox = true;
    return inst;
}

inline il::Instruction makeJump(il::BlockId target, std::vector<il::ValueId> args = {}) {
    il::Instruction inst;
    inst.op = il::Op::Jump;
    inst.type = il::Type::Void;
    inst.target.block = target;
    inst.target.args = std::move(args);
    return inst;
}

inline il::Instruction makeBranch(il::ValueId cond, il::BlockId thenBlock, il::BlockId elseBlock,
                           std::vector<il::ValueId> elseArgs = {}) {
    il::Instruction inst;
    inst.op = il::Op::Branch;
    inst.type = il::Type::Void;
    inst.operands = {cond};
    inst.target.block = thenBlock;
    inst.elseTarget.block = elseBlock;
    inst.elseTarget.args = std::move(elseArgs);
    return inst;
}


// RE-ENTRY (guard_region.h): the blocks between the slow copy's back edges and
// the fast copy's header.
struct ReentryBlocks {
    // One block per test: the entry-guarded values, then the header's
    // promoted parameters.
    std::vector<il::BlockId> tests;
    // Where every test has passed: the promoted parameters unboxed.
    il::BlockId conv = il::kNoBlock;
    // The fast header's one predecessor outside the fast copy, which the
    // entry edge reaches too.
    il::BlockId pre = il::kNoBlock;
    il::BlockId first() const { return tests.empty() ? conv : tests.front(); }
};

// Fills the re-entry blocks of `out`. `header` is the source header, whose
// parameters the slow copy's back edges pass; `promoted[k]` says parameter k
// is a double in the fast copy; `entryGuarded` are the values tested on the
// entry edge and `entryUnboxed` (with their doubles' names, `rawOf`) the ones
// whose double the fast copy reads. A failed test goes on to `slowHeader`.
// Returns the `is.number` tests emitted.
uint32_t emitReentry(std::vector<il::Block>& out, const ReentryBlocks& blocks, const il::Block& header,
                     const std::vector<uint8_t>& promoted, const std::vector<il::ValueId>& entryGuarded,
                     const std::vector<il::ValueId>& entryUnboxed, const std::vector<il::ValueId>& rawOf,
                     il::BlockId slowHeader, il::BlockId fastHeader, const std::function<il::ValueId()>& newValue);

}  // namespace bronze::lower

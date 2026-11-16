#include "opt/speculate/guard_builder.h"

#include <cassert>
#include <utility>

#include "il/il_op.h"
#include "opt/core/cfg_edit.h"
#include "opt/core/uses.h"

namespace bronze::opt::speculate {

GuardBuilder::GuardBuilder(RewriteContext& ctx, BlockId block, uint32_t index)
    : ctx_(ctx), fn_(ctx.speculated), siteBlock_(block), siteIndex_(index) {
    assert(block < fn_.blocks.size() && index + 1 < fn_.blocks[block].instructions.size() &&
           "a site is a non-terminator of its block");
    assert(mayPlaceResumePoint(ctx.generic, block));
    // The site in S is still G's instruction at the same place: the driver
    // rewrites a block's sites last to first, so splitting at one never moves
    // another still to come.
    tail_ = splitBlockBefore(fn_, block, index);
    il::Block& tail = fn_.blocks[tail_];
    site_ = tail.instructions.front();
    tail.instructions.erase(tail.instructions.begin());
    // The split's own jump goes: the cursor is `block`, open again.
    fn_.blocks[block].instructions.pop_back();
    cursor_ = block;
}

il::Type GuardBuilder::typeOf(ValueId v) const {
    return v < ctx_.types.size() ? ctx_.types[v] : il::Type::Void;
}

ValueId GuardBuilder::freshValue(il::Type type) {
    const ValueId v = newValue(fn_);
    if (ctx_.types.size() <= v) ctx_.types.resize(static_cast<size_t>(v) + 1, il::Type::Void);
    ctx_.types[v] = type;
    return v;
}

void GuardBuilder::append(il::Instruction inst) {
    assert(!finished_ && cursor_ != il::kNoBlock);
    assert(!isTerminator(inst.op));
    inst.span = site_.span;
    fn_.blocks[cursor_].instructions.push_back(std::move(inst));
}

void GuardBuilder::terminate(il::Instruction inst) {
    assert(!finished_ && cursor_ != il::kNoBlock);
    assert(isTerminator(inst.op));
    inst.span = site_.span;
    fn_.blocks[cursor_].instructions.push_back(std::move(inst));
    cursor_ = il::kNoBlock;
}

ValueId GuardBuilder::emit(il::Op op, il::Type type, std::vector<ValueId> operands, int32_t imm) {
    il::Instruction inst;
    inst.op = op;
    inst.type = type;
    inst.operands = std::move(operands);
    inst.immI32 = imm;
    return emitInst(std::move(inst));
}

ValueId GuardBuilder::emitInst(il::Instruction inst) {
    if (inst.type != il::Type::Void && inst.result == il::kNoValue) inst.result = freshValue(inst.type);
    const ValueId result = inst.result;
    append(std::move(inst));
    return result;
}

void GuardBuilder::emitResult(il::Instruction inst) {
    assert(site_.result != il::kNoValue && !resultDefined_);
    assert(inst.type == site_.type && "a rewrite keeps the site's result type");
    inst.result = site_.result;
    resultDefined_ = true;
    append(std::move(inst));
}

ValueId GuardBuilder::constI32(int32_t v) { return emit(il::Op::ConstI32, il::Type::I32, {}, v); }

ValueId GuardBuilder::constF64(double v) {
    il::Instruction inst;
    inst.op = il::Op::ConstF64;
    inst.type = il::Type::F64;
    inst.immF64 = v;
    return emitInst(std::move(inst));
}

ValueId GuardBuilder::constUndefined() { return emit(il::Op::ConstUndefined, il::Type::Dynamic, {}); }

ValueId GuardBuilder::box(ValueId v) {
    const il::Type t = typeOf(v);
    if (t == il::Type::Dynamic) return v;
    assert(t != il::Type::Void && t != il::Type::Ptr && "only a value boxes");
    il::Instruction inst;
    inst.op = il::Op::Box;
    inst.type = il::Type::Dynamic;
    inst.boxType = t;
    inst.operands = {v};
    return emitInst(std::move(inst));
}

BlockId GuardBuilder::newBlock() {
    const BlockId handler = fn_.blocks[siteBlock_].handler;
    const bool cold = fn_.blocks[siteBlock_].cold;
    il::Block& block = opt::newBlock(fn_);
    block.handler = handler;
    block.cold = cold;
    return block.id;
}

ValueId GuardBuilder::addParam(BlockId block, il::Type type, ValueId id) {
    if (id == il::kNoValue) {
        id = freshValue(type);
    } else {
        if (ctx_.types.size() <= id) ctx_.types.resize(static_cast<size_t>(id) + 1, il::Type::Void);
        ctx_.types[id] = type;
    }
    fn_.blocks[block].params.push_back(il::BlockParam{id, type});
    return id;
}

void GuardBuilder::resultParam(BlockId block) {
    assert(site_.result != il::kNoValue && !resultDefined_);
    addParam(block, site_.type, site_.result);
    resultDefined_ = true;
}

void GuardBuilder::branch(ValueId cond, BlockId thenBlock, std::vector<ValueId> thenArgs, BlockId elseBlock,
                          std::vector<ValueId> elseArgs) {
    assert(typeOf(cond) == il::Type::Bool && thenBlock != elseBlock);
    il::Instruction br;
    br.op = il::Op::Branch;
    br.operands = {cond};
    br.target = il::BlockTarget{thenBlock, std::move(thenArgs)};
    br.elseTarget = il::BlockTarget{elseBlock, std::move(elseArgs)};
    terminate(std::move(br));
}

void GuardBuilder::jump(BlockId target, std::vector<ValueId> args) {
    il::Instruction j;
    j.op = il::Op::Jump;
    j.target = il::BlockTarget{target, std::move(args)};
    terminate(std::move(j));
}

void GuardBuilder::moveTo(BlockId block) {
    assert(fn_.blocks[block].instructions.empty() || !isTerminator(fn_.blocks[block].instructions.back().op));
    cursor_ = block;
}

void GuardBuilder::guard(ValueId cond, bool passWhen) { guardTo(cond, passWhen, exitBefore()); }

void GuardBuilder::guardTo(ValueId cond, bool passWhen, BlockId exit) {
    const BlockId fast = newBlock();
    if (passWhen) {
        branch(cond, fast, {}, exit, {});
    } else {
        branch(cond, exit, {}, fast, {});
    }
    ++ctx_.stats.guards;
    cursor_ = fast;
}

BlockId GuardBuilder::makeExit(uint32_t resumeId, std::vector<il::Instruction> work, ValueId replaced,
                               ValueId replacement) {
    const BlockId exit = newBlock();
    fn_.blocks[exit].cold = true;
    for (il::Instruction& inst : work) {
        assert(!isTerminator(inst.op));
        inst.span = site_.span;
        fn_.blocks[exit].instructions.push_back(std::move(inst));
    }
    il::Instruction leave;
    leave.op = il::Op::Leave;
    leave.type = il::Type::Void;
    leave.calleeIndex = ctx_.genericIndex;
    leave.immI32 = static_cast<int32_t>(resumeId);
    // G's ids name S's copies (resume_points.h); only the site's result,
    // which an exit after the site has under another name, is substituted.
    leave.operands = ctx_.resume.state(resumeId);
    for (ValueId& v : leave.operands) {
        if (v == replaced) v = replacement;
    }
    leave.span = site_.span;
    fn_.blocks[exit].instructions.push_back(std::move(leave));
    return exit;
}

BlockId GuardBuilder::exitBefore() {
    if (exitBefore_ == il::kNoBlock) {
        const uint32_t id = ctx_.resume.at(siteBlock_, siteIndex_);
        exitBefore_ = makeExit(id, {}, il::kNoValue, il::kNoValue);
    }
    return exitBefore_;
}

uint32_t GuardBuilder::resumeAfterSite() {
    // In front of the instruction after the site: always one, the block's
    // terminator at the latest, since a site is not a terminator.
    return ctx_.resume.at(siteBlock_, siteIndex_ + 1);
}

BlockId GuardBuilder::exitAfter(std::vector<il::Instruction> work, ValueId replacement) {
    assert(site_.result == il::kNoValue || typeOf(replacement) == site_.type);
    return makeExit(resumeAfterSite(), std::move(work), site_.result, replacement);
}

void GuardBuilder::finish() {
    assert(!finished_);
    assert((site_.result == il::kNoValue || resultDefined_) && "the site's result is defined on the fast path");
    jump(tail_);
    finished_ = true;
}

}  // namespace bronze::opt::speculate

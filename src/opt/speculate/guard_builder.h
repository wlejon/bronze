#pragma once

// THE GUARD BUILDER: the one way a site of S is rewritten, used by every site
// kind.
//
// Opening a builder on instruction `index` of S's block `b` splits `b` in
// front of the site (opt/core splitBlockBefore): `b` keeps what came before
// and becomes the CURSOR, the site instruction comes out, and what came after
// it (the terminator included) waits in a tail block. The rewrite emits its
// sequence at the cursor; `finish` ends the cursor with a jump to the tail.
// Every block the builder makes keeps the site block's handler, so anything
// in the sequence that throws goes where the generic instruction's throw went.
//
// A GUARD is `br cond, fast, exit`: the cursor moves to `fast`, a new block;
// `exit` is cold and ends in `leave @G, id, state...`. All guards of a site
// that resume in front of it share ONE exit block and ONE resume point, placed
// in G in front of the generic instruction (resume_points.h), so a failed
// guard re-executes exactly that instruction in G. That is exact only when
// nothing the sequence did before the failing guard is observable: every
// sequence here does tests, header loads and slot loads before its last guard
// and nothing else, which each site file states for its own sequence.
//
// The site's RESULT keeps its id: the rewrite defines it on the fast path
// (`emitResult`, or a join parameter made with `resultParam`), so S's uses of
// it, and the state of every later exit, are untouched.

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "opt/speculate/context.h"

namespace bronze::opt::speculate {

class GuardBuilder {
public:
    GuardBuilder(RewriteContext& ctx, BlockId block, uint32_t index);
    GuardBuilder(const GuardBuilder&) = delete;
    GuardBuilder& operator=(const GuardBuilder&) = delete;

    // The generic instruction being replaced, as it stood in G (and in S).
    const il::Instruction& site() const { return site_; }
    BlockId cursor() const { return cursor_; }
    il::Type typeOf(ValueId v) const;
    RewriteContext& context() { return ctx_; }

    // ---- emitting at the cursor -------------------------------------------
    // A new instruction with a fresh result of `type` (none for Void); the
    // span is the site's. Returns the result, or kNoValue.
    ValueId emit(il::Op op, il::Type type, std::vector<ValueId> operands, int32_t imm = 0);
    // `inst` as given, its result made fresh when its type is not Void and it
    // names none.
    ValueId emitInst(il::Instruction inst);
    // The instruction that defines the site's own result id.
    void emitResult(il::Instruction inst);
    ValueId constI32(int32_t v);
    ValueId constF64(double v);
    ValueId constUndefined();
    ValueId box(ValueId v);  // `box.<type of v>`; a Dynamic v is returned as is
    // A fresh value id of `type`, for an instruction built by hand (an exit's
    // work).
    ValueId freshValue(il::Type type);

    // ---- guards ---------------------------------------------------------------
    // Continues at a new block when `cond` (bool) equals `passWhen`, and
    // leaves for the point in front of the site otherwise.
    void guard(ValueId cond, bool passWhen = true);
    // The same with the failing edge going to `exit` (a block from
    // exitBefore / exitAfter).
    void guardTo(ValueId cond, bool passWhen, BlockId exit);
    // The shared exit leaving in front of the site, made on first use.
    BlockId exitBefore();
    // An exit that does `work` (instructions with no effect G could see:
    // `proto.publish`) and leaves in front of the site.
    BlockId exitBeforeWith(std::vector<il::Instruction> work);
    // An exit that does `work` (generic instructions finishing what the site
    // started, appended in order) and leaves for the point in front of the
    // instruction AFTER the site, with `replacement` standing for the site's
    // result in the state. For a site whose first half already ran in S.
    BlockId exitAfter(std::vector<il::Instruction> work, ValueId replacement);
    // The id of G's resume point in front of the instruction AFTER the site
    // (the one exitAfter leaves to), made on first ask: the point a direct
    // call the site becomes records as its `resumeAfter` (il.h), so the
    // inliner can finish the call generically and resume G after it. Always
    // placeable: the site's block already admitted the point in front of it,
    // and the rule (resume_rules.h) is per block.
    uint32_t resumeAfterSite();
    // A cold block that runs the site's generic instruction (an IC op takes
    // a fresh IC site) and jumps to `join`, passing its result when the site
    // has one (`join` then carries the site's result as its parameter). The
    // arm for evidence that is incomplete rather than wrong: a polymorphic
    // site meeting one more receiver stays in S. `work` (instructions with no
    // effect G could see: `member.publish`) runs first.
    BlockId genericArm(BlockId join, std::vector<il::Instruction> work = {});

    // ---- blocks ---------------------------------------------------------------
    // A new block with the site's handler, cold when the site's block is.
    BlockId newBlock();
    // A parameter of `block`, fresh unless `id` names one.
    ValueId addParam(BlockId block, il::Type type, ValueId id = il::kNoValue);
    // A parameter of `block` that DEFINES the site's result.
    void resultParam(BlockId block);
    void branch(ValueId cond, BlockId thenBlock, std::vector<ValueId> thenArgs, BlockId elseBlock,
                std::vector<ValueId> elseArgs);
    void jump(BlockId target, std::vector<ValueId> args = {});
    // Moves the cursor to `block`, which must not be terminated yet.
    void moveTo(BlockId block);

    // Ends the rewrite: the cursor jumps to the tail. The site's result must
    // be defined by then.
    void finish();

private:
    void append(il::Instruction inst);
    void terminate(il::Instruction inst);
    BlockId makeExit(uint32_t resumeId, std::vector<il::Instruction> work, ValueId replaced,
                     ValueId replacement);

    RewriteContext& ctx_;
    il::Function& fn_;
    BlockId siteBlock_;
    uint32_t siteIndex_;
    il::Instruction site_;
    BlockId tail_ = il::kNoBlock;
    BlockId cursor_ = il::kNoBlock;
    BlockId exitBefore_ = il::kNoBlock;
    bool resultDefined_ = false;
    bool finished_ = false;
};

}  // namespace bronze::opt::speculate

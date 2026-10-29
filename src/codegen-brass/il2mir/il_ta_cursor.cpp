// How long a typed-array cursor lives (il_property_lowering.cpp
// get_preloaded_ta_cursor): a cursor is the raw address of an array's
// elements, which a collection moves, so it is forgotten wherever one could
// have run since it was derived. That is at every IL block's start, since
// the edge into a block may be a loop's back edge and every back edge
// carries an interrupt poll at which a compartment's fiber can be suspended
// while the rest of the thread collects; and after any instruction whose
// lowering emitted a call, a safepoint or a loop of its own, which gets a
// poll too. Within the rest of an IL block the MIR is a chain of diamonds
// the first access dominates, so one derivation serves every access after it.

#include "il_lowering.h"
#include <brass/mir/function.hpp>
#include <brass/mir/gc_refs.hpp>
#include <unordered_map>
#include <vector>

namespace il2mir {

namespace {

// Whether `inst` and everything after it in its block (or the whole block,
// from a null `inst`) is free of points where a collection can run.
bool cannot_collect_from(const BasicBlock* bb, const Instruction* first) {
    for (const Instruction* i = first ? first : bb->head(); i; i = i->next()) {
        if (brass::may_trigger_gc(*i)) return false;
    }
    return true;
}

// Whether the blocks the lowering touched form a cycle: a loop the lowering
// built, whose back edge brass polls.
bool has_cycle(const std::vector<BasicBlock*>& touched) {
    std::unordered_map<const BasicBlock*, uint8_t> state;  // 0 unseen, 1 on the stack, 2 done
    for (const BasicBlock* bb : touched) state[bb] = 0;
    struct Frame {
        const BasicBlock* bb;
        brass::SuccessorList succs;
        size_t next;
    };
    std::vector<Frame> stack;
    for (const BasicBlock* root : touched) {
        if (state[root] != 0) continue;
        state[root] = 1;
        stack.push_back({root, root->successors(), 0});
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.next == f.succs.size()) {
                state[f.bb] = 2;
                stack.pop_back();
                continue;
            }
            const BasicBlock* succ = f.succs[f.next++];
            auto it = state.find(succ);
            if (it == state.end()) continue;  // outside what the lowering touched
            if (it->second == 1) return true;
            if (it->second == 0) {
                it->second = 1;
                stack.push_back({succ, succ->successors(), 0});
            }
        }
    }
    return false;
}

}  // namespace

IlLowering::MirMark IlLowering::mir_mark(Builder& b, const Function& fn) const {
    MirMark m;
    m.block = b.current_block();
    m.tail = m.block ? m.block->tail() : nullptr;
    m.block_count = fn.blocks().size();
    return m;
}

void IlLowering::forget_ta_cursors() {
    for (auto& entry : ta_cursors_) {
        entry.second.data = nullptr;
    }
}

void IlLowering::settle_ta_cursors(const MirMark& mark, const Function& fn) {
    bool any = false;
    for (const auto& entry : ta_cursors_) any = any || entry.second.data != nullptr;
    if (!any || !mark.block) return;
    bool safe = cannot_collect_from(mark.block, mark.tail ? mark.tail->next() : nullptr);
    std::vector<BasicBlock*> touched{mark.block};
    const auto& blocks = fn.blocks();
    for (size_t i = mark.block_count; safe && i < blocks.size(); ++i) {
        safe = cannot_collect_from(blocks[i], nullptr);
        touched.push_back(blocks[i]);
    }
    if (!safe || has_cycle(touched)) forget_ta_cursors();
}

}  // namespace il2mir

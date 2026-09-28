// What a function's lowering decides once, at its entry: the values every
// inline path shares (hoisted), and which of its blocks run once
// (IlLowering::run_once).

#include "il_lowering.h"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace il2mir {

void IlLowering::begin_hoisting(BasicBlock* block) {
    hoist_block_ = block;
    hoist_after_ = block ? block->tail() : nullptr;
    // An empty block: the values go in front of whatever it gets later.
    hoist_at_head_ = block && !hoist_after_;
    for (Value*& v : hoisted_) v = nullptr;
}

Value* IlLowering::hoisted(Builder& b, Hoist kind) {
    Value*& slot = hoisted_[static_cast<size_t>(kind)];
    if (slot) return slot;
    // With no entry to hoist into (a wrapper), each use computes its own.
    BasicBlock* resume = b.current_block();
    if (hoist_block_) {
        if (hoist_after_) {
            b.position_after(hoist_after_);
        } else if (hoist_at_head_ && hoist_block_->head()) {
            b.position_before(hoist_block_->head());
        } else {
            b.position_at_end(hoist_block_);
        }
    }
    Value* v = nullptr;
    switch (kind) {
        case Hoist::IcTable:
            v = module_data_addr(b, "__bronze_ic_table");
            break;
        case Hoist::Count:
            break;
    }
    if (!hoist_block_) {
        return v;
    }
    hoist_after_ = v->defining_instruction();
    b.position_at_end(resume);
    slot = v;
    return v;
}

namespace {

// The IL successors of `blk`: its branch targets and its handler.
template <typename F>
void for_each_successor(const BronzeBlock& blk, F&& f) {
    for (const auto& inst : blk.instructions) {
        if (inst.op == BronzeOp::Jump) f(inst.target.block_id);
        if (inst.op == BronzeOp::Branch) {
            f(inst.target.block_id);
            f(inst.else_target.block_id);
        }
    }
    if (blk.handler_id != UINT32_MAX) f(blk.handler_id);
}

} // namespace

void IlLowering::analyze_function(const BronzeFunction& fn_ast) {
    run_once_ = false;
    run_once_blocks_.clear();
    fresh_objects_.clear();
    if (!small_forms_) return;
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::CreateObject && inst.result_id != UINT32_MAX) fresh_objects_.insert(inst.result_id);
        }
    }
    if (!fn_ast.is_toplevel || fn_ast.blocks.empty()) return;

    // The blocks on no cycle of the CFG: its strongly connected components
    // of one block without a self edge (Tarjan's, iteratively — a top level
    // has thousands of blocks).
    const size_t n = fn_ast.blocks.size();
    std::unordered_map<uint32_t, uint32_t> index_of;
    index_of.reserve(n);
    for (size_t i = 0; i < n; ++i) index_of.emplace(fn_ast.blocks[i].id, static_cast<uint32_t>(i));
    std::vector<std::vector<uint32_t>> succ(n);
    std::vector<uint8_t> self_edge(n, 0);
    for (size_t i = 0; i < n; ++i) {
        for_each_successor(fn_ast.blocks[i], [&](uint32_t id) {
            const auto it = index_of.find(id);
            if (it == index_of.end()) return;
            succ[i].push_back(it->second);
            if (it->second == i) self_edge[i] = 1;
        });
    }
    constexpr uint32_t kUnvisited = UINT32_MAX;
    std::vector<uint32_t> order(n, kUnvisited), low(n, 0);
    std::vector<uint8_t> on_stack(n, 0);
    std::vector<uint32_t> stack;
    std::vector<std::pair<uint32_t, size_t>> work;
    uint32_t counter = 0;
    for (uint32_t root = 0; root < n; ++root) {
        if (order[root] != kUnvisited) continue;
        work.emplace_back(root, 0);
        while (!work.empty()) {
            auto& [v, next] = work.back();
            if (next == 0 && order[v] == kUnvisited) {
                order[v] = low[v] = counter++;
                stack.push_back(v);
                on_stack[v] = 1;
            }
            if (next < succ[v].size()) {
                const uint32_t w = succ[v][next++];
                if (order[w] == kUnvisited) {
                    work.emplace_back(w, 0);
                } else if (on_stack[w]) {
                    low[v] = std::min(low[v], order[w]);
                }
                continue;
            }
            if (low[v] == order[v]) {
                size_t members = 0;
                uint32_t w = 0;
                do {
                    w = stack.back();
                    stack.pop_back();
                    on_stack[w] = 0;
                    ++members;
                } while (w != v);
                if (members == 1 && !self_edge[v]) run_once_blocks_.insert(fn_ast.blocks[v].id);
            }
            const uint32_t done = v;
            work.pop_back();
            if (!work.empty()) {
                const uint32_t parent = work.back().first;
                low[parent] = std::min(low[parent], low[done]);
            }
        }
    }
}

} // namespace il2mir

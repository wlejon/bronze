#include "lower/guard_region_cfg.h"

#include <algorithm>

namespace bronze::lower {

namespace {

void addEdge(Cfg& cfg, il::BlockId from, il::BlockId to) {
    if (to == il::kNoBlock || to >= cfg.succs.size()) return;
    auto& s = cfg.succs[from];
    if (std::find(s.begin(), s.end(), to) == s.end()) s.push_back(to);
    auto& p = cfg.preds[to];
    if (std::find(p.begin(), p.end(), from) == p.end()) p.push_back(from);
}

// Depth-first postorder from the entry, iteratively: a three.js function can be
// hundreds of blocks deep through a chain of `if`s and recursion here would be
// a stack the compiler's own depth limit does not cover.
void computeRpo(Cfg& cfg) {
    const size_t n = cfg.succs.size();
    std::vector<uint8_t> seen(n, 0);
    std::vector<il::BlockId> postorder;
    std::vector<std::pair<il::BlockId, size_t>> stack;
    if (n == 0) return;
    seen[0] = 1;
    stack.push_back({0, 0});
    while (!stack.empty()) {
        auto& [block, next] = stack.back();
        if (next < cfg.succs[block].size()) {
            const il::BlockId succ = cfg.succs[block][next++];
            if (!seen[succ]) {
                seen[succ] = 1;
                stack.push_back({succ, 0});
            }
            continue;
        }
        postorder.push_back(block);
        stack.pop_back();
    }
    cfg.rpo.assign(postorder.rbegin(), postorder.rend());
    cfg.rpoIndex.assign(n, UINT32_MAX);
    for (uint32_t i = 0; i < cfg.rpo.size(); ++i) cfg.rpoIndex[cfg.rpo[i]] = i;
}

// Cooper-Harvey-Kennedy: walk the two idom chains toward the entry by reverse
// postorder number until they meet.
il::BlockId intersect(const Cfg& cfg, il::BlockId a, il::BlockId b) {
    while (a != b) {
        while (cfg.rpoIndex[a] > cfg.rpoIndex[b]) {
            a = cfg.idom[a];
            if (a == il::kNoBlock) return b;
        }
        while (cfg.rpoIndex[b] > cfg.rpoIndex[a]) {
            b = cfg.idom[b];
            if (b == il::kNoBlock) return a;
        }
    }
    return a;
}

}  // namespace

bool Cfg::dominates(il::BlockId a, il::BlockId b) const {
    if (a == b) return true;
    if (b >= idom.size() || rpoIndex[b] == UINT32_MAX) return false;
    for (il::BlockId cur = idom[b]; cur != il::kNoBlock; cur = idom[cur]) {
        if (cur == a) return true;
    }
    return false;
}

Cfg buildCfg(const il::Function& fn) {
    Cfg cfg;
    const size_t n = fn.blocks.size();
    cfg.succs.resize(n);
    cfg.preds.resize(n);
    for (size_t b = 0; b < n; ++b) {
        const auto& block = fn.blocks[b];
        addEdge(cfg, static_cast<il::BlockId>(b), block.handler);
        if (block.instructions.empty()) continue;
        const auto& term = block.instructions.back();
        if (term.op == il::Op::Jump) {
            addEdge(cfg, static_cast<il::BlockId>(b), term.target.block);
        } else if (term.op == il::Op::Branch) {
            addEdge(cfg, static_cast<il::BlockId>(b), term.target.block);
            addEdge(cfg, static_cast<il::BlockId>(b), term.elseTarget.block);
        }
    }

    computeRpo(cfg);
    cfg.idom.assign(n, il::kNoBlock);
    if (cfg.rpo.empty()) return cfg;
    cfg.idom[0] = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (il::BlockId b : cfg.rpo) {
            if (b == 0) continue;
            il::BlockId newIdom = il::kNoBlock;
            for (il::BlockId p : cfg.preds[b]) {
                if (cfg.rpoIndex[p] == UINT32_MAX || cfg.idom[p] == il::kNoBlock) continue;
                newIdom = newIdom == il::kNoBlock ? p : intersect(cfg, newIdom, p);
            }
            if (newIdom != il::kNoBlock && cfg.idom[b] != newIdom) {
                cfg.idom[b] = newIdom;
                changed = true;
            }
        }
    }
    // The entry dominates itself, but naming itself as its own immediate
    // dominator makes every chain walk above infinite.
    cfg.idom[0] = il::kNoBlock;
    return cfg;
}

std::vector<il::BlockId> naturalLoop(const Cfg& cfg, il::BlockId header, il::BlockId tail) {
    std::vector<uint8_t> inLoop(cfg.preds.size(), 0);
    inLoop[header] = 1;
    std::vector<il::BlockId> work;
    if (tail != header) {
        inLoop[tail] = 1;
        work.push_back(tail);
    }
    while (!work.empty()) {
        const il::BlockId b = work.back();
        work.pop_back();
        for (il::BlockId p : cfg.preds[b]) {
            if (inLoop[p]) continue;
            inLoop[p] = 1;
            work.push_back(p);
        }
    }
    std::vector<il::BlockId> out;
    for (size_t b = 0; b < inLoop.size(); ++b) {
        if (inLoop[b]) out.push_back(static_cast<il::BlockId>(b));
    }
    return out;
}

namespace {

// Every value one instruction READS: its operands, and the arguments it hands
// to the blocks it jumps to. A block parameter is not a use of anything.
template <typename Fn>
void forEachUse(const il::Instruction& inst, Fn&& fn) {
    for (il::ValueId id : inst.operands) fn(id);
    for (il::ValueId id : inst.target.args) fn(id);
    for (il::ValueId id : inst.elseTarget.args) fn(id);
}

}  // namespace

std::vector<std::vector<il::ValueId>> computeLiveIn(const il::Function& fn, const Cfg& cfg) {
    const size_t n = fn.blocks.size();
    const uint32_t values = fn.valueCount;
    std::vector<std::vector<uint8_t>> live(n, std::vector<uint8_t>(values, 0));

    bool changed = true;
    while (changed) {
        changed = false;
        // Backwards over reverse postorder, which for a reducible CFG reaches
        // the fixpoint in about two sweeps instead of one per loop depth.
        for (auto it = cfg.rpo.rbegin(); it != cfg.rpo.rend(); ++it) {
            const il::BlockId b = *it;
            std::vector<uint8_t> cur(values, 0);
            for (il::BlockId s : cfg.succs[b]) {
                for (uint32_t v = 0; v < values; ++v) cur[v] |= live[s][v];
            }
            const auto& block = fn.blocks[b];
            for (auto inst = block.instructions.rbegin(); inst != block.instructions.rend();
                 ++inst) {
                if (inst->result != il::kNoValue && inst->result < values) cur[inst->result] = 0;
                forEachUse(*inst, [&](il::ValueId id) {
                    if (id < values) cur[id] = 1;
                });
            }
            for (const auto& param : block.params) {
                if (param.id < values) cur[param.id] = 0;
            }
            if (cur != live[b]) {
                live[b] = std::move(cur);
                changed = true;
            }
        }
    }

    std::vector<std::vector<il::ValueId>> out(n);
    for (size_t b = 0; b < n; ++b) {
        for (uint32_t v = 0; v < values; ++v) {
            if (live[b][v]) out[b].push_back(v);
        }
    }
    return out;
}

std::vector<il::ValueId> liveBefore(const il::Function& fn, const Cfg& cfg,
                                    const std::vector<std::vector<il::ValueId>>& liveIn,
                                    il::BlockId blockId, uint32_t index) {
    const uint32_t values = fn.valueCount;
    std::vector<uint8_t> cur(values, 0);
    for (il::BlockId s : cfg.succs[blockId]) {
        for (il::ValueId v : liveIn[s]) {
            if (v < values) cur[v] = 1;
        }
    }
    const auto& block = fn.blocks[blockId];
    for (size_t i = block.instructions.size(); i-- > index;) {
        const auto& inst = block.instructions[i];
        if (inst.result != il::kNoValue && inst.result < values) cur[inst.result] = 0;
        forEachUse(inst, [&](il::ValueId id) {
            if (id < values) cur[id] = 1;
        });
    }
    std::vector<il::ValueId> out;
    for (uint32_t v = 0; v < values; ++v) {
        if (cur[v]) out.push_back(v);
    }
    return out;
}

std::vector<DefSite> computeDefSites(const il::Function& fn) {
    std::vector<DefSite> sites(fn.valueCount);
    for (size_t p = 0; p < fn.params.size(); ++p) {
        if (p < sites.size()) sites[p] = DefSite{il::kNoBlock, UINT32_MAX};
    }
    for (const auto& block : fn.blocks) {
        for (const auto& param : block.params) {
            if (param.id < sites.size()) sites[param.id] = DefSite{block.id, UINT32_MAX};
        }
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            if (inst.result != il::kNoValue && inst.result < sites.size()) {
                sites[inst.result] = DefSite{block.id, static_cast<uint32_t>(i)};
            }
        }
    }
    return sites;
}

}  // namespace bronze::lower

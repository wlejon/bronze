#include "opt/core/liveness.h"

#include <bit>
#include <cstddef>

#include "opt/core/uses.h"

namespace bronze::opt {

namespace {

inline void setBit(uint64_t* set, ValueId v) { set[v >> 6] |= uint64_t{1} << (v & 63); }
inline void clearBit(uint64_t* set, ValueId v) { set[v >> 6] &= ~(uint64_t{1} << (v & 63)); }

// The largest id the function names plus one, so a count that lags the ids
// still sizes the sets correctly.
size_t idBound(const il::Function& fn) {
    size_t n = fn.valueCount;
    if (fn.params.size() > n) n = fn.params.size();
    auto grow = [&](ValueId v) {
        if (v != kNoValue && v >= n) n = static_cast<size_t>(v) + 1;
    };
    for (const il::Block& block : fn.blocks) {
        for (const il::BlockParam& p : block.params) grow(p.id);
        for (const il::Instruction& inst : block.instructions) {
            grow(inst.result);
            forEachUse(inst, grow);
        }
    }
    return n;
}

// One instruction, backwards: its result dies, its uses come alive.
inline void stepBack(uint64_t* live, const il::Instruction& inst) {
    if (inst.result != kNoValue) clearBit(live, inst.result);
    forEachUse(inst, [&](ValueId v) {
        if (v != kNoValue) setBit(live, v);
    });
}

}  // namespace

Liveness::Liveness(const il::Function& fn, const Cfg& cfg) {
    const size_t n = fn.blocks.size();
    words_ = (idBound(fn) + 63) / 64;
    in_.assign(n * words_, 0);
    out_.assign(n * words_, 0);
    if (n == 0 || words_ == 0) return;

    // Per block: GEN (used before any definition in the block) and KILL
    // (defined in the block, parameters included).
    Bits gen(n * words_, 0);
    Bits kill(n * words_, 0);
    for (size_t b = 0; b < n; ++b) {
        const il::Block& block = fn.blocks[b];
        uint64_t* g = &gen[b * words_];
        uint64_t* k = &kill[b * words_];
        for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
            stepBack(g, *it);
            if (it->result != kNoValue) setBit(k, it->result);
        }
        for (const il::BlockParam& p : block.params) {
            if (p.id == kNoValue) continue;
            clearBit(g, p.id);
            setBit(k, p.id);
        }
    }

    // Postorder over the reachable blocks first (fast convergence), then the
    // unreachable ones, which nothing flows into but which still have sets.
    std::vector<BlockId> order = cfg.postorder();
    for (size_t b = 0; b < n; ++b) {
        if (!cfg.reachable(static_cast<BlockId>(b))) order.push_back(static_cast<BlockId>(b));
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (BlockId b : order) {
            uint64_t* out = &out_[b * words_];
            for (BlockId s : cfg.succs(b)) {
                const uint64_t* sin = &in_[s * words_];
                for (size_t w = 0; w < words_; ++w) out[w] |= sin[w];
            }
            const uint64_t* g = &gen[b * words_];
            const uint64_t* k = &kill[b * words_];
            uint64_t* in = &in_[b * words_];
            for (size_t w = 0; w < words_; ++w) {
                const uint64_t next = g[w] | (out[w] & ~k[w]);
                if (next != in[w]) {
                    in[w] = next;
                    changed = true;
                }
            }
        }
    }
}

std::vector<ValueId> Liveness::toVector(const Bits& bits, BlockId b) const {
    std::vector<ValueId> out;
    const uint64_t* set = &bits[b * words_];
    for (size_t w = 0; w < words_; ++w) {
        uint64_t word = set[w];
        while (word != 0) {
            const int bit = std::countr_zero(word);
            out.push_back(static_cast<ValueId>(w * 64 + static_cast<size_t>(bit)));
            word &= word - 1;
        }
    }
    return out;
}

bool Liveness::test(const Bits& bits, BlockId b, ValueId v) const {
    if (v == kNoValue || (v >> 6) >= words_) return false;
    return (bits[b * words_ + (v >> 6)] >> (v & 63)) & 1;
}

std::vector<ValueId> Liveness::liveBefore(const il::Function& fn, BlockId b, size_t index) const {
    std::vector<uint64_t> live(out_.begin() + static_cast<std::ptrdiff_t>(b * words_),
                               out_.begin() + static_cast<std::ptrdiff_t>((b + 1) * words_));
    const auto& insts = fn.blocks[b].instructions;
    for (size_t i = insts.size(); i > index; --i) stepBack(live.data(), insts[i - 1]);
    std::vector<ValueId> out;
    for (size_t w = 0; w < words_; ++w) {
        uint64_t word = live[w];
        while (word != 0) {
            const int bit = std::countr_zero(word);
            out.push_back(static_cast<ValueId>(w * 64 + static_cast<size_t>(bit)));
            word &= word - 1;
        }
    }
    return out;
}

std::vector<ValueId> liveBefore(const il::Function& fn, BlockId b, size_t index) {
    const Cfg cfg(fn);
    const Liveness live(fn, cfg);
    return live.liveBefore(fn, b, index);
}

}  // namespace bronze::opt

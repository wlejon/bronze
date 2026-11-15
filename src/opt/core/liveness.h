#pragma once

// SSA liveness per block, and the live set in front of any instruction.
//
// A block parameter is defined at the block's entry; an edge's arguments are
// uses at the end of the block the edge leaves; a handler's live-in is live
// out of every block that throws to it (it can be entered from anywhere in
// the block, so whatever it needs must be live throughout). Function
// parameters are ordinary values defined before b0.
//
// `liveBefore` is what a `resume.point`'s state is made from: the values the
// code from that instruction on still needs, ascending by id, so the state of
// a point is a function of the IL alone.

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "opt/core/cfg.h"

namespace bronze::opt {

class Liveness {
public:
    Liveness(const il::Function& fn, const Cfg& cfg);

    // Ascending by id.
    std::vector<ValueId> liveIn(BlockId b) const { return toVector(in_, b); }
    std::vector<ValueId> liveOut(BlockId b) const { return toVector(out_, b); }
    bool isLiveIn(BlockId b, ValueId v) const { return test(in_, b, v); }
    bool isLiveOut(BlockId b, ValueId v) const { return test(out_, b, v); }

    // The values live in front of instruction `index` of block `b` (index ==
    // the instruction count means after the terminator: the live-out set,
    // which is the union over the edges taken). Ascending by id.
    std::vector<ValueId> liveBefore(const il::Function& fn, BlockId b, size_t index) const;

    size_t valueCount() const { return words_ * 64; }

private:
    using Bits = std::vector<uint64_t>;
    std::vector<ValueId> toVector(const Bits& bits, BlockId b) const;
    bool test(const Bits& bits, BlockId b, ValueId v) const;

    size_t words_ = 0;
    Bits in_;   // block-major: block b's set is words [b * words_, (b + 1) * words_)
    Bits out_;
};

// One-shot form: builds the Cfg and the Liveness and answers one query. A
// caller placing many points builds a Liveness once instead.
std::vector<ValueId> liveBefore(const il::Function& fn, BlockId b, size_t index);

}  // namespace bronze::opt

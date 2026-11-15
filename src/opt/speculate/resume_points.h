#pragma once

// THE RESUME POINTS of one generic function G, collected while its S is
// rewritten and written into G once at the end.
//
// A point is a location in G — in front of instruction `index` of `block`,
// counted in G as lowering left it — and its STATE, `liveBefore(G, block,
// index)`: the values the code from there on needs, ascending by id
// (opt/core liveness.h). Every query is answered from G before any point is
// inserted, so the indices and the liveness are G's own; the points then go
// in back to front in each block, which keeps the indices of the ones still
// to place valid. Two guards that resume at one location share one point.
//
// The state is in G's value ids. S keeps G's ids (cloneFunction), and every
// rewrite in S re-defines a replaced instruction's result under its old id
// on the fast path, so the same list names S's copies of those values at any
// exit in front of the location: a value live there was defined before it.

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "il/il.h"
#include "opt/core/cfg.h"
#include "opt/core/liveness.h"

namespace bronze::opt::speculate {

using il::BlockId;
using il::ValueId;

class ResumePoints {
public:
    explicit ResumePoints(const il::Function& generic);

    // The id of the point in front of instruction `index` of G's `block`,
    // made on first ask. The block must be one mayPlaceResumePoint allows.
    uint32_t at(BlockId block, uint32_t index);
    // Its state, in G's ids, in operand order.
    const std::vector<ValueId>& state(uint32_t id) const;
    // Whether a point already exists at the location (loop entries reuse a
    // site's point at a header's top).
    bool has(BlockId block, uint32_t index) const;

    size_t size() const { return points_.size(); }

    // Writes every point into G, which must be the function this was built
    // from, unchanged since. After this no query may be made.
    void insertInto(il::Function& generic);

private:
    struct Point {
        BlockId block = 0;
        uint32_t index = 0;
        uint32_t id = 0;
        std::vector<ValueId> state;
    };

    const il::Function& generic_;
    Cfg cfg_;
    Liveness live_;
    std::map<std::pair<BlockId, uint32_t>, uint32_t> byLocation_;
    std::vector<Point> points_;
    uint32_t nextId_ = 0;
    bool inserted_ = false;
};

}  // namespace bronze::opt::speculate

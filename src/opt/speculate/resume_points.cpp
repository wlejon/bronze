#include "opt/speculate/resume_points.h"

#include <algorithm>
#include <cassert>

namespace bronze::opt::speculate {

ResumePoints::ResumePoints(const il::Function& generic)
    : generic_(generic), cfg_(generic), live_(generic, cfg_) {
    // Ids are unique within G. A G that already carries points (none does
    // today: the step runs once, on lowering's output) keeps them, and the new
    // ones start past the largest.
    for (const il::Block& block : generic.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (inst.op == il::Op::ResumePoint && inst.immI32 >= 0) {
                nextId_ = std::max(nextId_, static_cast<uint32_t>(inst.immI32) + 1);
            }
        }
    }
}

uint32_t ResumePoints::at(BlockId block, uint32_t index) {
    assert(!inserted_ && "a resume point asked for after they were written into G");
    assert(block < generic_.blocks.size() && index < generic_.blocks[block].instructions.size());
    const auto key = std::make_pair(block, index);
    if (auto it = byLocation_.find(key); it != byLocation_.end()) return it->second;
    Point p;
    p.block = block;
    p.index = index;
    p.id = nextId_++;
    p.state = live_.liveBefore(generic_, block, index);
    byLocation_.emplace(key, p.id);
    points_.push_back(std::move(p));
    return points_.back().id;
}

bool ResumePoints::has(BlockId block, uint32_t index) const {
    return byLocation_.count(std::make_pair(block, index)) != 0;
}

const std::vector<ValueId>& ResumePoints::state(uint32_t id) const {
    for (const Point& p : points_) {
        if (p.id == id) return p.state;
    }
    assert(false && "state of a resume point this function never made");
    static const std::vector<ValueId> none;
    return none;
}

void ResumePoints::insertInto(il::Function& generic) {
    assert(&generic == &generic_ && !inserted_);
    inserted_ = true;
    // Back to front within a block, so an insertion never moves a location
    // still to be filled. Two points at one location cannot happen (one id
    // per location); the order among blocks does not matter.
    std::vector<const Point*> order;
    order.reserve(points_.size());
    for (const Point& p : points_) order.push_back(&p);
    std::sort(order.begin(), order.end(), [](const Point* a, const Point* b) {
        if (a->block != b->block) return a->block < b->block;
        return a->index > b->index;
    });
    for (const Point* p : order) {
        il::Block& block = generic.blocks[p->block];
        il::Instruction rp;
        rp.op = il::Op::ResumePoint;
        rp.type = il::Type::Void;
        rp.result = il::kNoValue;
        rp.immI32 = static_cast<int32_t>(p->id);
        rp.operands = p->state;
        rp.span = block.instructions[p->index].span;
        block.instructions.insert(block.instructions.begin() + static_cast<std::ptrdiff_t>(p->index),
                                  std::move(rp));
    }
}

}  // namespace bronze::opt::speculate

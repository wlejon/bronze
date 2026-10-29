#pragma once

// The cycles of an IL function's CFG: which blocks can run more than once in
// one call of it.

#include "il_ast.h"
#include <cstdint>
#include <vector>

namespace il2mir {

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

// Per block of `fn` (by position in fn.blocks): 1 when it is on a cycle — a
// strongly connected component of more than one block, or a block with an
// edge to itself.
std::vector<uint8_t> cyclic_blocks(const BronzeFunction& fn);

} // namespace il2mir

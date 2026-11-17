#pragma once

// THE ELEMENT CELLS of a module: one cell per computed read (`elem.get`) in
// the module's per-thread data (`__bronze_elem_cells`, five u64 per cell:
// shape, key bits, slot word, fill count, validity; bronze_abi_layout.h
// BRONZE_ABI_ELEM_CELL_WORDS, runtime/elem_ic.cpp bronze_elem_get_cell). A
// read's cell is named by its slow site
// (il.h Instruction::slowSite), which every copy of the read shares, so G's
// read and S's copy of it use one cell; a read with no slow site has none.
// An `instanceof` has a cell of the same size and its own meaning
// (bronze_abi_layout.h BRONZE_ABI_INSTANCEOF_CELL_*, il_prop_stubs.cpp
// build_instanceof_stub).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::codegen {

// The slow sites of the module's computed reads, sorted and distinct: cell n
// belongs to the read whose slow site is element n.
inline std::vector<uint32_t> elemCellSites(const il::Module& module) {
    std::vector<uint32_t> sites;
    for (const il::Function& fn : module.functions) {
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if ((inst.op == il::Op::ElemGet || inst.op == il::Op::InstanceOf) &&
                    inst.slowSite != il::Instruction::kNoSlowSite) {
                    sites.push_back(inst.slowSite);
                }
            }
        }
    }
    std::sort(sites.begin(), sites.end());
    sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
    return sites;
}

}  // namespace bronze::codegen

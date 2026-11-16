#include "opt/speculate/context.h"

#include <string>

namespace bronze::opt::speculate {

std::vector<il::Type> valueTypes(const il::Function& fn) {
    std::vector<il::Type> types(fn.valueCount, il::Type::Void);
    auto set = [&](ValueId v, il::Type t) {
        if (v < types.size()) types[v] = t;
    };
    for (size_t p = 0; p < fn.params.size(); ++p) set(static_cast<ValueId>(p), fn.params[p].type);
    for (const il::Block& block : fn.blocks) {
        for (const il::BlockParam& bp : block.params) set(bp.id, bp.type);
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result != il::kNoValue) set(inst.result, inst.type);
        }
    }
    return types;
}

ShapeCells::ShapeCells(il::Module& module) : module_(module) {
    for (size_t i = 0; i < module.shapeCells.size(); ++i) {
        byKey_.emplace(module.shapeCells[i].key, static_cast<uint32_t>(i));
    }
}

uint32_t ShapeCells::cellFor(const std::string& key) {
    if (auto it = byKey_.find(key); it != byKey_.end()) return it->second;
    const auto index = static_cast<uint32_t>(module_.shapeCells.size());
    module_.shapeCells.push_back(il::Module::ShapeCell{key});
    byKey_.emplace(key, index);
    ++added_;
    return index;
}

uint32_t ShapeCells::memberCellFor(il::Module::MemberKind kind, uint32_t keyIndex, uint32_t callee,
                                   const std::string& shape) {
    const std::string id = std::to_string(static_cast<uint32_t>(kind)) + ":" + std::to_string(keyIndex) + ":" +
                           std::to_string(callee) + ":" + shape;
    if (auto it = members_.find(id); it != members_.end()) return it->second;
    const auto index = static_cast<uint32_t>(module_.memberCells.size());
    il::Module::MemberCell cell;
    cell.kind = kind;
    cell.keyIndex = keyIndex;
    cell.callee = callee;
    cell.group = index;
    cell.groupSize = 1;
    module_.memberCells.push_back(cell);
    members_.emplace(id, index);
    return index;
}

}  // namespace bronze::opt::speculate

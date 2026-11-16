#include "opt/core/clone.h"

#include <algorithm>

namespace bronze::opt {

void CloneMap::mapValue(ValueId from, ValueId to) {
    if (from == kNoValue) return;
    if (from >= values.size()) values.resize(static_cast<size_t>(from) + 1, kNoValue);
    values[from] = to;
}

void CloneMap::mapBlock(BlockId from, BlockId to) {
    if (from == kNoBlock) return;
    if (from >= blocks.size()) blocks.resize(static_cast<size_t>(from) + 1, kNoBlock);
    blocks[from] = to;
}

uint32_t cloneFunction(il::Module& module, uint32_t fnIndex, const std::string& name, CloneMap* map) {
    // Copy before appending: the deque keeps references, but a copy taken
    // from a reference while the deque grows is not worth reasoning about.
    il::Function copy = module.functions[fnIndex];
    copy.name = name;
    copy.isExported = false;
    copy.isEntryPoint = false;
    if (map != nullptr) {
        map->values.resize(copy.valueCount);
        for (ValueId v = 0; v < copy.valueCount; ++v) map->values[v] = v;
        map->blocks.resize(copy.blocks.size());
        for (BlockId b = 0; b < copy.blocks.size(); ++b) map->blocks[b] = b;
    }
    module.functions.push_back(std::move(copy));
    return static_cast<uint32_t>(module.functions.size() - 1);
}

void cloneBlocks(const il::Function& src, const std::vector<BlockId>& blocks, il::Function& dst,
                 CloneMap& map) {
    // Snapshot the sources: when src is dst, appending moves the blocks.
    std::vector<il::Block> copies;
    copies.reserve(blocks.size());
    for (BlockId b : blocks) copies.push_back(src.blocks[b]);

    // The set's own ids and definitions get fresh names first, so a use that
    // precedes its definition in the given order still finds the copy.
    std::vector<uint8_t> inSet(src.blocks.size(), 0);
    for (BlockId b : blocks) inSet[b] = 1;
    BlockId nextBlock = static_cast<BlockId>(dst.blocks.size());
    std::vector<ValueId> localValues(src.valueCount, kNoValue);
    std::vector<BlockId> localBlocks(src.blocks.size(), kNoBlock);
    for (const il::Block& block : copies) {
        localBlocks[block.id] = nextBlock++;
        for (const il::BlockParam& p : block.params) {
            if (p.id < localValues.size()) localValues[p.id] = dst.valueCount++;
        }
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result != kNoValue && inst.result < localValues.size()) {
                localValues[inst.result] = dst.valueCount++;
            }
        }
    }

    auto value = [&](ValueId v) {
        if (v < localValues.size() && localValues[v] != kNoValue) return localValues[v];
        const ValueId seeded = map.value(v);
        return seeded != kNoValue ? seeded : v;
    };
    auto block = [&](BlockId b) {
        if (b == kNoBlock) return b;
        if (b < inSet.size() && inSet[b]) return localBlocks[b];
        const BlockId seeded = map.block(b);
        return seeded != kNoBlock ? seeded : b;
    };

    for (il::Block& copy : copies) {
        const BlockId oldId = copy.id;
        copy.id = localBlocks[oldId];
        copy.handler = block(copy.handler);
        for (il::BlockParam& p : copy.params) p.id = value(p.id);
        for (il::Instruction& inst : copy.instructions) {
            if (inst.result != kNoValue) inst.result = value(inst.result);
            for (ValueId& v : inst.operands) v = value(v);
            for (ValueId& v : inst.target.args) v = value(v);
            for (ValueId& v : inst.elseTarget.args) v = value(v);
            if (inst.op == il::Op::Jump || inst.op == il::Op::Branch) {
                inst.target.block = block(inst.target.block);
            }
            if (inst.op == il::Op::Branch) inst.elseTarget.block = block(inst.elseTarget.block);
        }
    }

    for (size_t v = 0; v < localValues.size(); ++v) {
        if (localValues[v] != kNoValue) map.mapValue(static_cast<ValueId>(v), localValues[v]);
    }
    for (BlockId b : blocks) map.mapBlock(b, localBlocks[b]);
    for (il::Block& copy : copies) dst.blocks.push_back(std::move(copy));
}

size_t removeResumePoints(il::Function& fn) {
    size_t removed = 0;
    for (il::Block& block : fn.blocks) {
        auto& insts = block.instructions;
        const auto end = std::remove_if(insts.begin(), insts.end(), [](const il::Instruction& inst) {
            return inst.op == il::Op::ResumePoint;
        });
        removed += static_cast<size_t>(insts.end() - end);
        insts.erase(end, insts.end());
    }
    return removed;
}

namespace {

uint32_t* codeReference(il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::Call:
        case il::Op::CreateFunction:
        case il::Op::FunctionRef:
        case il::Op::FnCodeIs:
            return &inst.calleeIndex;
        case il::Op::Construct:
            return inst.directTarget != il::Instruction::kNoDirectTarget ? &inst.directTarget : nullptr;
        default:
            return nullptr;
    }
}

}  // namespace

uint32_t redirectFunctionReferences(il::Module& module, const std::vector<uint32_t>& entry) {
    uint32_t redirected = 0;
    for (il::Function& fn : module.functions) {
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) {
                uint32_t* index = codeReference(inst);
                if (index == nullptr || *index >= entry.size() || entry[*index] == *index) continue;
                *index = entry[*index];
                ++redirected;
            }
        }
    }
    // A member cell holds the function whose code its method runs, which the
    // runtime compares with a receiver's: the same code the closures run.
    for (il::Module::MemberCell& cell : module.memberCells) {
        if (cell.callee >= entry.size() || entry[cell.callee] == cell.callee) continue;
        cell.callee = entry[cell.callee];
        ++redirected;
    }
    return redirected;
}

}  // namespace bronze::opt

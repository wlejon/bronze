#include "lower/loop_prop_hoist.h"

#include <algorithm>
#include <vector>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

bool isImpureForLoopPropHoist(const il::Instruction& inst) {
    switch (inst.op) {
        // Property / Element / Private Stores and Mutators
        case il::Op::PropSet:
        case il::Op::ElemSet:
        case il::Op::ElemSetTyped:
        case il::Op::SuperSet:
        case il::Op::PrivateSet:
        case il::Op::PrivateAdd:
        case il::Op::MethodDef:
        case il::Op::MethodDefComputed:
        case il::Op::AccessorDef:
        case il::Op::AccessorDefComputed:
        case il::Op::DefineOwnAttr:
        case il::Op::PropDelete:
        case il::Op::ElemDelete:
        case il::Op::ClassExtend:
        // Environment Writes
        case il::Op::EnvSet:
        case il::Op::ModuleEnvSet:
        case il::Op::EnvInitTdz:
        case il::Op::ImmutableAssign:
        // Calls, Construction & Dynamic Import
        case il::Op::Call:
        case il::Op::DynamicCall:
        case il::Op::DynamicCallSpread:
        case il::Op::Construct:
        case il::Op::ConstructSpread:
        case il::Op::MethodCall:
        case il::Op::MethodCallSpread:
        case il::Op::SuperCall:
        case il::Op::SuperCallSpread:
        case il::Op::DynamicImport:
        // Control flow effects / Suspension
        case il::Op::Throw:
        case il::Op::AsyncAwait:
        case il::Op::AsyncIterOpen:
        case il::Op::AsyncIterNext:
        case il::Op::AsyncIterClose:
        case il::Op::IterDelegate:
            return true;
        default:
            return false;
    }
}

bool hoistInFunction(il::Function& fn, const std::vector<uint8_t>& isAccessorKey) {
    if (fn.blocks.empty()) return false;

    bool fnChanged = false;
    bool keepGoing = true;
    while (keepGoing) {
        keepGoing = false;
        const Cfg cfg = buildCfg(fn);
        std::vector<DefSite> defs = computeDefSites(fn);

        // Find loop headers and their backedges
        for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
            std::vector<il::BlockId> backedges;
            for (il::BlockId pred : cfg.preds[header]) {
                if (cfg.dominates(header, pred)) {
                    backedges.push_back(pred);
                }
            }
            if (backedges.empty()) continue;

            // Form the natural loop as the union of loop blocks for all backedges to header
            std::vector<uint8_t> inLoop(fn.blocks.size(), 0);
            std::vector<il::BlockId> loopBlocks;
            for (il::BlockId tail : backedges) {
                const auto nat = naturalLoop(cfg, header, tail);
                for (il::BlockId b : nat) {
                    if (b < inLoop.size() && !inLoop[b]) {
                        inLoop[b] = 1;
                        loopBlocks.push_back(b);
                    }
                }
            }

            // Check if L has a unique preheader P:
            // P is an outside predecessor (cfg.preds[header] has an edge from P not in L),
            // and all other predecessors of header are inside L.
            il::BlockId preheader = il::kNoBlock;
            bool uniquePreheader = true;
            for (il::BlockId pred : cfg.preds[header]) {
                if (!inLoop[pred]) {
                    if (preheader == il::kNoBlock) {
                        preheader = pred;
                    } else {
                        uniquePreheader = false;
                        break;
                    }
                }
            }

            if (!uniquePreheader || preheader == il::kNoBlock || preheader >= fn.blocks.size()) {
                continue;
            }

            // P terminates with a Jump to header
            il::Block& pBlock = fn.blocks[preheader];
            if (pBlock.instructions.empty()) continue;
            const auto& term = pBlock.instructions.back();
            if (term.op != il::Op::Jump || term.target.block != header) {
                continue;
            }

            // Verify Loop Purity: loop must contain NO stores and NO calls
            bool pure = true;
            for (il::BlockId b : loopBlocks) {
                if (fn.blocks[b].handler != il::kNoBlock) {
                    pure = false;
                    break;
                }
                for (const auto& inst : fn.blocks[b].instructions) {
                    if (isImpureForLoopPropHoist(inst)) {
                        pure = false;
                        break;
                    }
                }
                if (!pure) break;
            }
            if (!pure) continue;

            // Find Loop-Invariant PropGets
            bool loopModified = false;
            for (il::BlockId b : loopBlocks) {
                il::Block& blk = fn.blocks[b];
                for (size_t i = 0; i < blk.instructions.size();) {
                    auto& inst = blk.instructions[i];
                    if (inst.op == il::Op::PropGet) {
                        if (inst.keyIndex < isAccessorKey.size() && isAccessorKey[inst.keyIndex]) {
                            ++i;
                            continue;
                        }
                        const il::ValueId recv = inst.operands[0];
                        const bool isInvariant =
                            recv < defs.size() &&
                            (defs[recv].block == il::kNoBlock || !inLoop[defs[recv].block]);
                        if (isInvariant) {
                            // Hoist into preheader immediately before terminating Jump
                            il::Instruction hoisted = inst;
                            const il::ValueId oldVal = inst.result;
                            const il::ValueId newVal = fn.valueCount++;
                            hoisted.result = newVal;

                            pBlock.instructions.insert(pBlock.instructions.end() - 1, hoisted);
                            if (defs.size() <= newVal) {
                                defs.resize(newVal + 1);
                            }
                            defs[newVal] = DefSite{
                                preheader, static_cast<uint32_t>(pBlock.instructions.size() - 2)};

                            // Replace all uses of oldVal in fn with newVal
                            for (il::Block& targetBlk : fn.blocks) {
                                for (il::Instruction& userInst : targetBlk.instructions) {
                                    for (il::ValueId& op : userInst.operands) {
                                        if (op == oldVal) op = newVal;
                                    }
                                    for (il::ValueId& arg : userInst.target.args) {
                                        if (arg == oldVal) arg = newVal;
                                    }
                                    for (il::ValueId& arg : userInst.elseTarget.args) {
                                        if (arg == oldVal) arg = newVal;
                                    }
                                }
                            }

                            // Erase original PropGet
                            blk.instructions.erase(blk.instructions.begin() + i);
                            loopModified = true;
                            fnChanged = true;
                            continue;
                        }
                    }
                    ++i;
                }
            }

            if (loopModified) {
                keepGoing = true;
                break;  // Rebuild CFG and def sites for next round
            }
        }
    }
    return fnChanged;
}

bool eliminateRedundantPropGets(il::Function& fn, const std::vector<uint8_t>& isAccessorKey) {
    struct Avail {
        il::ValueId recv;
        uint32_t keyIndex;
        il::ValueId val;
    };
    bool changed = false;
    for (il::Block& blk : fn.blocks) {
        std::vector<Avail> avail;
        for (size_t i = 0; i < blk.instructions.size();) {
            auto& inst = blk.instructions[i];
            if (isImpureForLoopPropHoist(inst)) {
                avail.clear();
                ++i;
                continue;
            }
            if (inst.op == il::Op::PropGet && inst.result != il::kNoValue) {
                const il::ValueId recv = inst.operands[0];
                const uint32_t key = inst.keyIndex;
                if (key < isAccessorKey.size() && !isAccessorKey[key]) {
                    il::ValueId existing = il::kNoValue;
                    for (const auto& a : avail) {
                        if (a.recv == recv && a.keyIndex == key) {
                            existing = a.val;
                            break;
                        }
                    }
                    if (existing != il::kNoValue) {
                        const il::ValueId oldVal = inst.result;
                        for (il::Block& targetBlk : fn.blocks) {
                            for (il::Instruction& userInst : targetBlk.instructions) {
                                for (il::ValueId& op : userInst.operands) {
                                    if (op == oldVal) op = existing;
                                }
                                for (il::ValueId& arg : userInst.target.args) {
                                    if (arg == oldVal) arg = existing;
                                }
                                for (il::ValueId& arg : userInst.elseTarget.args) {
                                    if (arg == oldVal) arg = existing;
                                }
                            }
                        }
                        blk.instructions.erase(blk.instructions.begin() + i);
                        changed = true;
                        continue;
                    }
                    avail.push_back({recv, key, inst.result});
                }
            }
            ++i;
        }
    }
    return changed;
}

}  // namespace

bool hoistLoopInvariantProps(il::Module& module) {
    bool moduleUsesProxy = false;
    bool moduleHasDynamicAccessors = false;
    std::vector<uint8_t> isAccessorKey(module.keyConstants.size(), 0);

    for (size_t k = 0; k < module.keyConstants.size(); ++k) {
        const std::string& name = module.keyConstants[k];
        if (name == "Proxy") {
            moduleUsesProxy = true;
        }
        if (name == "Reflect" || name == "defineProperty" || name == "defineProperties") {
            moduleHasDynamicAccessors = true;
        }
        if (name == "size" || name == "flags" || name == "source" || name == "buffer" ||
            name == "byteLength" || name == "byteOffset" || name == "__proto__") {
            isAccessorKey[k] = 1;
        }
    }

    for (const il::Function& fn : module.functions) {
        for (const il::Block& blk : fn.blocks) {
            for (const il::Instruction& inst : blk.instructions) {
                if (inst.op == il::Op::AccessorDef) {
                    if (inst.keyIndex < isAccessorKey.size()) {
                        isAccessorKey[inst.keyIndex] = 1;
                    }
                } else if (inst.op == il::Op::AccessorDefComputed) {
                    moduleHasDynamicAccessors = true;
                }
            }
        }
    }

    if (moduleUsesProxy || moduleHasDynamicAccessors) {
        return false;
    }

    bool anyChanged = false;
    for (il::Function& fn : module.functions) {
        if (eliminateRedundantPropGets(fn, isAccessorKey)) {
            anyChanged = true;
        }
        if (hoistInFunction(fn, isAccessorKey)) {
            anyChanged = true;
        }
    }
    return anyChanged;
}

}  // namespace bronze::lower

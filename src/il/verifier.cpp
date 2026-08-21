#include "il/verifier.h"

#include <unordered_map>
#include <unordered_set>

namespace bronze::il {

bool verifyFunction(const Function& fn, DiagnosticSink& diags) {
    if (fn.blocks.empty()) {
        diags.error(Span{}, "Function " + fn.name + " has no blocks");
        return false;
    }

    if (fn.blocks[0].id != 0) {
        diags.error(Span{}, "Function " + fn.name + " entry block id is not 0");
        return false;
    }

    if (!fn.blocks[0].params.empty()) {
        diags.error(Span{}, "Function " + fn.name + " entry block b0 must not have block parameters");
        return false;
    }

    // Map of ValueId -> defined Type
    std::unordered_map<ValueId, Type> definedTypes;
    // Map of ValueId -> {block_index, instruction_index_in_block (or -1 if param)}
    struct DefLocation {
        size_t blockIdx;
        int instIdx;
    };
    std::unordered_map<ValueId, DefLocation> defLocations;

    auto recordDef = [&](ValueId id, Type type, size_t blockIdx, int instIdx) -> bool {
        // `kNoValue` is the "there is no value here" sentinel, never a name.
        // Letting it become a definition is what made every use of it legal:
        // one default-constructed BlockParam put UINT32_MAX into this map,
        // and from then on `jump b1(%4294967295)` type-checked and reached
        // codegen. Rejected here so the sentinel can never be looked up.
        if (id == kNoValue) {
            diags.error(Span{}, "Function " + fn.name +
                                    ": the no-value sentinel %" + std::to_string(kNoValue) +
                                    " is used as a definition");
            return false;
        }
        if (definedTypes.contains(id)) {
            diags.error(Span{}, "Function " + fn.name + ": duplicate definition of %" + std::to_string(id));
            return false;
        }
        definedTypes[id] = type;
        defLocations[id] = DefLocation{blockIdx, instIdx};
        return true;
    };

    // 1. Record function parameters (%0 .. %N-1)
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (!recordDef(static_cast<ValueId>(i), fn.params[i].type, 0, -1)) {
            return false;
        }
    }

    // 2. Record block parameters & instruction results
    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
        const auto& block = fn.blocks[bIdx];
        if (block.id != static_cast<BlockId>(bIdx)) {
            diags.error(Span{}, "Function " + fn.name + ": block id mismatch at index " +
                                  std::to_string(bIdx) + " (expected b" + std::to_string(bIdx) +
                                  ", got b" + std::to_string(block.id) + ")");
            return false;
        }

        // A handler is a real edge the backend materializes, and it is entered
        // from the middle of a block — so it can carry no arguments, and there
        // is therefore nowhere for a parameter's incoming value to come from.
        // Both halves are checked here because lowering is the only producer
        // and a violation would surface as an LLVM phi with a missing incoming,
        // which names nothing.
        if (block.handler != kNoBlock) {
            if (block.handler >= fn.blocks.size()) {
                diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) +
                                        " names handler b" + std::to_string(block.handler) +
                                        ", which is out of range");
                return false;
            }
            if (!fn.blocks[block.handler].params.empty()) {
                diags.error(Span{}, "Function " + fn.name + ": handler b" +
                                        std::to_string(block.handler) +
                                        " has block parameters, which no edge into a handler can "
                                        "supply");
                return false;
            }
        }

        for (size_t pIdx = 0; pIdx < block.params.size(); ++pIdx) {
            const auto& param = block.params[pIdx];
            // A block parameter is an SSA definition, so it needs a real type
            // for the incoming arguments to be checked against. `Void` is the
            // default-constructed state and means the producer never filled it
            // in — every argument would then have to be Void too, which no
            // instruction produces.
            if (param.type == Type::Void) {
                diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) +
                                        " parameter " + std::to_string(pIdx) + " has type void");
                return false;
            }
            if (!recordDef(param.id, param.type, bIdx, -1)) {
                return false;
            }
        }

        for (size_t iIdx = 0; iIdx < block.instructions.size(); ++iIdx) {
            const auto& inst = block.instructions[iIdx];
            if (inst.result != kNoValue) {
                if (!recordDef(inst.result, inst.type, bIdx, static_cast<int>(iIdx))) {
                    return false;
                }
            }
        }
    }

    // Helper to check use of a value
    auto checkUse = [&](ValueId id, size_t useBlockIdx, size_t useInstIdx) -> bool {
        auto it = definedTypes.find(id);
        if (it == definedTypes.end()) {
            diags.error(Span{}, "Function " + fn.name + ": use of undefined value %" + std::to_string(id));
            return false;
        }
        const auto& loc = defLocations[id];
        if (loc.blockIdx == useBlockIdx && loc.instIdx >= static_cast<int>(useInstIdx)) {
            diags.error(Span{}, "Function " + fn.name + ": within-block use-after-def of %" + std::to_string(id));
            return false;
        }
        return true;
    };

    // Block arguments are the SSA join, so they carry the same obligations as
    // any other use: one argument per target block parameter, each a value
    // defined somewhere, each of the parameter's type. An argument list that
    // satisfies none of that becomes an LLVM phi incoming value, which is why
    // this is a hard error rather than a lint.
    auto checkTarget = [&](const BlockTarget& target, size_t useBlockIdx, size_t useInstIdx) -> bool {
        if (target.block >= fn.blocks.size()) {
            diags.error(Span{}, "Function " + fn.name + ": branch target b" +
                                  std::to_string(target.block) + " out of range");
            return false;
        }
        const auto& tgtBlock = fn.blocks[target.block];
        if (target.args.size() != tgtBlock.params.size()) {
            diags.error(Span{}, "Function " + fn.name + ": target b" + std::to_string(target.block) +
                                  " expects " + std::to_string(tgtBlock.params.size()) +
                                  " arguments, got " + std::to_string(target.args.size()));
            return false;
        }
        for (size_t i = 0; i < target.args.size(); ++i) {
            ValueId argId = target.args[i];
            // Named separately from "undefined value": a `kNoValue` argument
            // is a producer that had no value to pass and passed the sentinel
            // anyway (an env-backed or uninitialised binding reaching an edge
            // it should never reach), not a typo'd id.
            if (argId == kNoValue) {
                diags.error(Span{}, "Function " + fn.name + ": argument " + std::to_string(i) +
                                        " to b" + std::to_string(target.block) +
                                        " is the no-value sentinel %" + std::to_string(kNoValue));
                return false;
            }
            if (!checkUse(argId, useBlockIdx, useInstIdx)) return false;
            if (definedTypes[argId] != tgtBlock.params[i].type) {
                diags.error(Span{}, "Function " + fn.name + ": type mismatch for argument " +
                                      std::to_string(i) + " passed to b" + std::to_string(target.block) +
                                      " (argument is " + typeName(definedTypes[argId]) +
                                      ", parameter is " + typeName(tgtBlock.params[i].type) + ")");
                return false;
            }
        }
        return true;
    };

    // 3. Verify instructions and terminators
    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
        const auto& block = fn.blocks[bIdx];
        if (block.instructions.empty()) {
            diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) + " has no instructions");
            return false;
        }

        for (size_t iIdx = 0; iIdx < block.instructions.size(); ++iIdx) {
            const auto& inst = block.instructions[iIdx];
            bool isLast = (iIdx == block.instructions.size() - 1);
            bool term = isTerminator(inst.op);

            if (term && !isLast) {
                diags.error(Span{}, "Function " + fn.name + ": terminator inside block b" +
                                      std::to_string(bIdx) + " before last instruction");
                return false;
            }
            if (!term && isLast) {
                diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) +
                                      " does not end with a terminator");
                return false;
            }

            for (ValueId opId : inst.operands) {
                if (!checkUse(opId, bIdx, iIdx)) return false;
            }

            if (inst.op == Op::Throw) {
                if (inst.operands.size() != 1) {
                    diags.error(Span{}, "Function " + fn.name +
                                            ": throw takes exactly one operand");
                    return false;
                }
                if (definedTypes[inst.operands[0]] != Type::Dynamic) {
                    diags.error(Span{}, "Function " + fn.name + ": throw operand %" +
                                            std::to_string(inst.operands[0]) + " is " +
                                            typeName(definedTypes[inst.operands[0]]) +
                                            ", not dynamic (any value can be thrown)");
                    return false;
                }
            }

            if (inst.op == Op::Jump) {
                if (!checkTarget(inst.target, bIdx, iIdx)) return false;
            } else if (inst.op == Op::Branch) {
                if (inst.operands.empty()) {
                    diags.error(Span{}, "Function " + fn.name + ": br missing condition operand");
                    return false;
                }
                ValueId condId = inst.operands[0];
                if (definedTypes[condId] != Type::Bool) {
                    diags.error(Span{}, "Function " + fn.name + ": br condition %" +
                                          std::to_string(condId) + " is not bool");
                    return false;
                }
                if (inst.target.block == inst.elseTarget.block) {
                    diags.error(Span{}, "Function " + fn.name + ": br has identical then and else target b" +
                                          std::to_string(inst.target.block));
                    return false;
                }
                if (!checkTarget(inst.target, bIdx, iIdx)) return false;
                if (!checkTarget(inst.elseTarget, bIdx, iIdx)) return false;
            }
        }
    }

    return true;
}

bool verify(const Module& module, DiagnosticSink& diags) {
    for (const auto& fn : module.functions) {
        if (!verifyFunction(fn, diags)) {
            return false;
        }
    }

    // A closure's environment reaches it through the dynamic calling
    // convention, which a direct `call` does not use — so a direct call to a
    // function that needs an environment would enter it with garbage in the
    // environment parameter. Lowering routes every call to a closure through
    // call.dynamic; this checks that rather than trusting it.
    for (const auto& fn : module.functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                const bool refersToFunction = inst.op == Op::Call ||
                                              inst.op == Op::CreateFunction ||
                                              inst.op == Op::FunctionRef;
                if (!refersToFunction) continue;
                if (inst.calleeIndex >= module.functions.size()) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                            " names an out-of-range function index");
                    return false;
                }
                if (inst.op != Op::Call) continue;
                const Function& callee = module.functions[inst.calleeIndex];
                if (callee.needsEnv) {
                    diags.error(Span{}, "Function " + fn.name + ": direct call to closure @" +
                                            callee.name + ", which needs an environment");
                    return false;
                }
            }
        }
    }

    // The IC table is a fixed-size global array in the generated object file,
    // so an icIndex past the module's site count is an out-of-bounds store into
    // the object file's data, not a missed optimization. Named here rather than
    // clamped in the backend.
    for (const auto& fn : module.functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == Op::PropGet || inst.op == Op::PropSet ||
                    inst.op == Op::MethodCall || inst.op == Op::MethodCallSpread) {
                    if (inst.icIndex >= module.icSiteCount) {
                        diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                                " names inline-cache site " +
                                                std::to_string(inst.icIndex) +
                                                ", past the module's site count " +
                                                std::to_string(module.icSiteCount));
                        return false;
                    }
                    // Same shape of bug, same rule: the static-slot cell array
                    // is a fixed-size global the backend emits, so a cell index
                    // past the count is an out-of-bounds store.
                    // A FAMILY site names no cell at all — its guard is a
                    // range compare against constants — so the bound is the
                    // identity form's alone.
                    if (inst.staticSlot != Instruction::kNoStaticSlot &&
                        inst.familyLo == Instruction::kNoFamily &&
                        inst.staticCellIndex >= module.staticSiteCount) {
                        diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                                " names static-slot site " +
                                                std::to_string(inst.staticCellIndex) +
                                                ", past the module's site count " +
                                                std::to_string(module.staticSiteCount));
                        return false;
                    }
                    // And a family site's range has to be inside the table the
                    // module actually emits, for the same reason: the runtime
                    // registers exactly this many classes.
                    if (inst.familyLo != Instruction::kNoFamily &&
                        static_cast<size_t>(inst.familyLo) + inst.familySpan >=
                            module.classFamilies.size()) {
                        diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                                " names class-family range " +
                                                std::to_string(inst.familyLo) + ".." +
                                                std::to_string(inst.familyLo + inst.familySpan) +
                                                ", past the module's class count " +
                                                std::to_string(module.classFamilies.size()));
                        return false;
                    }
                    continue;
                }
                // The template-slot table is the same kind of fixed-size global
                // array, so an index past the count is the same out-of-bounds
                // store — checked here for the same reason and not in the
                // backend.
                if (inst.op != Op::TemplateCached && inst.op != Op::TemplateObject) continue;
                if (inst.immI32 < 0 ||
                    static_cast<uint32_t>(inst.immI32) >= module.templateSiteCount) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                            " names template site " +
                                            std::to_string(inst.immI32) +
                                            ", past the module's site count " +
                                            std::to_string(module.templateSiteCount));
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace bronze::il

#include "il/verifier_proofs.h"

#include <string>
#include <unordered_map>

#include "il/il_pin.h"

namespace bronze::il {

namespace {

bool isValidPinBarrier(int32_t raw) {
    return raw == static_cast<int32_t>(PinBarrier::Number) ||
           raw == static_cast<int32_t>(PinBarrier::NumberOrNullish) ||
           raw == static_cast<int32_t>(PinBarrier::DenseArray);
}

}  // namespace

bool verifyFunctionProofs(const Function& fn, const Module* module, DiagnosticSink& diags) {
    auto fail = [&](const std::string& msg) {
        diags.error(Span{}, "proof certificate refused in function " + fn.name + ": " + msg);
        return false;
    };

    // 1. Verify pinned parameter and return certificates on the function signature.
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Param& p = fn.params[i];
        if (!p.pinned) continue;
        if (p.type != Type::F64) {
            return fail("pinned parameter '" + p.name + "' has type " +
                        std::string(typeName(p.type)) + ", not f64");
        }
        if (module != nullptr && !module->keyConstants.empty() &&
            p.pinKeyIndex >= module->keyConstants.size()) {
            return fail("pinned parameter '" + p.name + "' names key index " +
                        std::to_string(p.pinKeyIndex) + ", past the module's key count " +
                        std::to_string(module->keyConstants.size()));
        }
    }
    if (fn.returnPinned) {
        if (fn.returnType != Type::F64) {
            return fail("returnPinned is set on function with return type " +
                        std::string(typeName(fn.returnType)) + ", not f64");
        }
        if (module != nullptr && !module->keyConstants.empty() &&
            fn.returnPinKeyIndex >= module->keyConstants.size()) {
            return fail("returnPinned names key index " + std::to_string(fn.returnPinKeyIndex) +
                        ", past the module's key count " +
                        std::to_string(module->keyConstants.size()));
        }
    }

    if (fn.isExternal || fn.blocks.empty()) return true;

    // 2. The type of each value, for the operand checks.
    std::unordered_map<ValueId, Type> types;
    for (size_t i = 0; i < fn.params.size(); ++i) types[static_cast<ValueId>(i)] = fn.params[i].type;
    for (const Block& blk : fn.blocks) {
        for (const BlockParam& bp : blk.params) types[bp.id] = bp.type;
        for (const Instruction& inst : blk.instructions) {
            if (inst.result == kNoValue) continue;
            types[inst.result] = inst.type;
        }
    }

    // 3. Each instruction's certificate, in block/instruction order.
    for (const Block& blk : fn.blocks) {
        for (const Instruction& inst : blk.instructions) {
            if (inst.op == Op::PinGuard) {
                if (inst.operands.size() != 1 || inst.result != kNoValue ||
                    inst.type != Type::Void) {
                    return fail("pin.guard takes exactly one operand and produces void");
                }
                const ValueId op0 = inst.operands[0];
                auto opIt = types.find(op0);
                if (opIt == types.end() || opIt->second != Type::Dynamic) {
                    return fail("pin.guard operand %" + std::to_string(op0) + " is not dynamic");
                }
                if (!isValidPinBarrier(inst.immI32)) {
                    return fail("pin.guard has invalid PinBarrier kind " +
                                std::to_string(inst.immI32));
                }
                if (module != nullptr && !module->keyConstants.empty() &&
                    inst.keyIndex >= module->keyConstants.size()) {
                    return fail("pin.guard names key index " + std::to_string(inst.keyIndex) +
                                ", past the module's key count " +
                                std::to_string(module->keyConstants.size()));
                }
            }
        }
    }

    return true;
}

bool verifyModuleProofs(const Module& module, DiagnosticSink& diags) {
    for (const Function& fn : module.functions) {
        if (!verifyFunctionProofs(fn, &module, diags)) {
            return false;
        }
    }
    return true;
}

}  // namespace bronze::il

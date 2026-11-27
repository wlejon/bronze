#include "il/feedback_site.h"

#include "abi/bronze_abi.h"

#include <map>
#include <string_view>
#include <utility>

namespace bronze::il {

bool observesOperandTags(const Instruction& inst) {
    switch (inst.op) {
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Pow:
        case Op::Neg: case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl:
        case Op::Shr: case Op::UShr: case Op::BitNot:
        case Op::ToNumeric: case Op::NumericStep: case Op::ToInt32:
        case Op::RelLt: case Op::RelGt: case Op::RelLe: case Op::RelGe:
        case Op::CmpLt: case Op::CmpGt: case Op::CmpLe: case Op::CmpGe: case Op::CmpEq:
        case Op::CmpNe: case Op::StrictEq: case Op::LooseEq:
            return true;
        case Op::Unbox:
            return true;
        default:
            return false;
    }
}

uint64_t icLayoutDigest(const Module& module) {
    uint64_t h = 1469598103934665603ull;
    auto mixByte = [&h](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    auto mix64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) mixByte(static_cast<uint8_t>(v >> (i * 8)));
    };
    auto mixText = [&](std::string_view s) {
        mix64(s.size());
        for (unsigned char c : s) mixByte(c);
    };
    mix64(module.sourceTexts.size());
    for (const std::string& text : module.sourceTexts) mixText(text);
    mix64(module.icSiteCount);
    mix64(module.slowSiteCount);
    // Index -> (op, key): the first instruction met in function, block and
    // instruction order. Copies of a site (inlining, versioning, threading)
    // share its index and say the same thing about it.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> named;
    for (const Function& fn : module.functions) {
        for (const Block& block : fn.blocks) {
            for (const Instruction& inst : block.instructions) {
                if (inst.op != Op::PropGet && inst.op != Op::PropSet && inst.op != Op::MethodCall &&
                    inst.op != Op::MethodCallSpread && inst.op != Op::ApplyArgs) {
                    continue;
                }
                named.emplace(inst.icIndex, std::make_pair(static_cast<uint32_t>(inst.op), inst.keyIndex));
            }
        }
    }
    for (const auto& [index, what] : named) {
        mix64(index);
        mix64(what.first);
        mixText(what.second < module.keyConstants.size() ? std::string_view(module.keyConstants[what.second])
                                                         : std::string_view("?"));
    }
    return h;
}

void numberSlowSites(Module& module) {
    uint32_t next = 0;
    for (Function& fn : module.functions) {
        // The ENTRY and LOOP COUNTS (feedback_site.h), no arms.
        if (!fn.isExternal && !fn.blocks.empty()) {
            fn.entrySlowSite = next;
            next += kFunctionSlowSites;
        }
        for (Block& block : fn.blocks) {
            for (Instruction& inst : block.instructions) {
                switch (inst.op) {
                    case Op::ElemGet: case Op::ElemSet: case Op::PropGet: case Op::PropSet:
                    case Op::StrictEq: case Op::Unbox:
                    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
                    case Op::Branch: case Op::DynamicCall: case Op::Construct: case Op::Call: case Op::MethodCall:
                    case Op::RelLt: case Op::RelGt: case Op::RelLe: case Op::RelGe: case Op::LooseEq:
                    case Op::ToNumeric: case Op::NumericStep: case Op::BitNot:
                    case Op::ObjectLiteral: case Op::InstanceOf:
                        inst.slowSite = next;
                        next += kSlowSiteArms;
                        break;
                    // The numeric ops with a typed form count only as their
                    // dynamic one, which has the inline arm and the helper.
                    case Op::Mod: case Op::Neg: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
                    case Op::Shl: case Op::Shr: case Op::UShr:
                        if (inst.type != Type::Dynamic) break;
                        inst.slowSite = next;
                        next += kSlowSiteArms;
                        break;
                    default:
                        break;
                }
            }
        }
    }
    module.slowSiteCount = next;
}

}  // namespace bronze::il

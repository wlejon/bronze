#include "opt/redundancy/licence.h"

namespace bronze::opt::redundancy {

std::optional<Licence> licenceAt(const il::Instruction& inst, const FactEnv& facts) {
    if (inst.result == kNoValue || inst.operands.size() != 1) return std::nullopt;
    const ValueId x = inst.operands[0];
    switch (inst.op) {
        // Being an object is a fact about the value: it never ends.
        case il::Op::LoadShape:
            if (!facts.isObject(x)) return std::nullopt;
            return Licence{LicenceKind::Object, x, 0, {}};
        // A shape lasts until a write of Shapes changes it.
        case il::Op::SlotLoad:
            if (const auto cell = facts.shapeOf(x)) return Licence{LicenceKind::Shape, x, *cell, il::Loc::Shapes};
            return std::nullopt;
        // A closure's code and captured record never change.
        case il::Op::FnEnv:
            if (const auto fn = facts.calleeOf(x)) return Licence{LicenceKind::Callee, x, *fn, {}};
            return std::nullopt;
        // An Array stays an Array; the facts that say so (dense, plain) are
        // memory facts, so the licence carries what they depend on.
        case il::Op::ArrayLength:
            if (!facts.isArray(x)) return std::nullopt;
            return Licence{LicenceKind::Array, x, 0, il::Loc::Shapes | il::Loc::ArrayLen | il::Loc::Elements};
        default:
            return std::nullopt;
    }
}

bool licenceHolds(const Licence& licence, const FactEnv& facts) {
    switch (licence.kind) {
        case LicenceKind::Object:
            return facts.isObject(licence.subject);
        case LicenceKind::Shape:
            return facts.shapeOf(licence.subject) == static_cast<uint32_t>(licence.imm);
        case LicenceKind::Callee:
            return facts.calleeOf(licence.subject) == static_cast<uint32_t>(licence.imm);
        case LicenceKind::Array:
            return facts.isArray(licence.subject);
    }
    return false;
}

}  // namespace bronze::opt::redundancy

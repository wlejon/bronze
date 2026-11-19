#include "opt/redundancy/licence.h"

#include <algorithm>

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
            if (auto cells = facts.shapeSet(x); cells && !cells->empty()) {
                return Licence{LicenceKind::Shape, x, 0, il::Loc::Shapes, std::move(*cells)};
            }
            return std::nullopt;
        // A closure's code and captured record never change.
        case il::Op::FnEnv:
            if (const auto fn = facts.calleeOf(x)) return Licence{LicenceKind::Callee, x, *fn, {}};
            return std::nullopt;
        // An Array stays an Array. `is.array` says so of the value; the other
        // facts that do (dense, plain) are memory facts, so the licence then
        // carries what they depend on.
        case il::Op::ArrayLength:
            if (facts.known(FactKind::Array, x) == true) return Licence{LicenceKind::Array, x, 0, {}};
            if (!facts.isArray(x)) return std::nullopt;
            return Licence{LicenceKind::Array, x, 0, il::Loc::Shapes | il::Loc::ArrayLen};
        // A typed array's kind never changes, and the bare kind test is a
        // fact about the value.
        case il::Op::TypedLength:
            if (!facts.isTypedArray(x)) return std::nullopt;
            return Licence{LicenceKind::TypedArray, x, 0, {}};
        default:
            return std::nullopt;
    }
}

bool licenceHolds(const Licence& licence, const FactEnv& facts) {
    switch (licence.kind) {
        case LicenceKind::Object:
            return facts.isObject(licence.subject);
        case LicenceKind::Shape:
        {
            const std::optional<std::vector<uint32_t>> cells = facts.shapeSet(licence.subject);
            return cells && std::includes(licence.cells.begin(), licence.cells.end(), cells->begin(), cells->end());
        }
        case LicenceKind::Callee:
            return facts.calleeOf(licence.subject) == static_cast<uint32_t>(licence.imm);
        case LicenceKind::Array:
            return facts.isArray(licence.subject);
        case LicenceKind::TypedArray:
            return facts.isTypedArray(licence.subject);
    }
    return false;
}

}  // namespace bronze::opt::redundancy

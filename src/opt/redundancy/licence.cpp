#include "opt/redundancy/licence.h"

#include <algorithm>

namespace bronze::opt::redundancy {

namespace {

struct ProtoChain {
    ValueId rootShape = kNoValue;
    uint32_t cell = 0;
    uint32_t depth = 0;
    uint32_t targetDepth = 0;
    int32_t targetSlot = 0;
};

std::optional<ProtoChain> protoChainAtShape(const il::Module& module, const il::Function& fn, const DefUse& defs,
                                            ValueId shape, const FactEnv& facts) {
    ValueId cur = shape;
    for (uint32_t hops = 0; hops < 16; ++hops) {
        for (uint32_t c : facts.memberCells(cur)) {
            if (c >= module.memberCells.size()) continue;
            const il::Module::MemberCell& mc = module.memberCells[c];
            if (mc.kind != il::Module::MemberKind::Data) continue;
            const uint32_t targetDepth = mc.callee >> il::Module::kMemberDataSlotBits;
            const auto targetSlot =
                static_cast<int32_t>(mc.callee & ((1u << il::Module::kMemberDataSlotBits) - 1));
            if (hops < targetDepth) {
                return ProtoChain{cur, c, hops, targetDepth, targetSlot};
            }
        }
        const il::Instruction* load = defs.defInstIs(fn, cur, il::Op::LoadShape);
        if (load == nullptr || load->operands.size() != 1) break;
        const il::Instruction* proto = defs.defInstIs(fn, load->operands[0], il::Op::ShapeProto);
        if (proto == nullptr || proto->operands.size() != 1) break;
        cur = proto->operands[0];
    }
    return std::nullopt;
}

}  // namespace

std::optional<Licence> licenceAt(const il::Module& module, const il::Function& fn, const DefUse& defs,
                                 const il::Instruction& inst, const FactEnv& facts) {
    if (inst.result == kNoValue || inst.operands.size() != 1) return std::nullopt;
    const ValueId x = inst.operands[0];
    switch (inst.op) {
        case il::Op::ShapeProto:
            if (const auto chain = protoChainAtShape(module, fn, defs, x, facts)) {
                return Licence{LicenceKind::MemberData, chain->rootShape, static_cast<int64_t>(chain->cell),
                               il::Loc::Protos};
            }
            return std::nullopt;
        // Being an object is a fact about the value: it never ends. Otherwise,
        // `load.shape` on a prototype link reached from a data `member.cell`.
        case il::Op::LoadShape:
            if (facts.isObject(x)) return Licence{LicenceKind::Object, x, 0, {}};
            if (const il::Instruction* proto = defs.defInstIs(fn, x, il::Op::ShapeProto);
                proto != nullptr && proto->operands.size() == 1) {
                if (const auto chain = protoChainAtShape(module, fn, defs, proto->operands[0], facts)) {
                    return Licence{LicenceKind::MemberData, chain->rootShape, static_cast<int64_t>(chain->cell),
                                   il::Loc::Protos};
                }
            }
            return std::nullopt;
        // A shape lasts until a write of Shapes changes it; a data `member.cell`
        // slot load lasts until `Protos` changes.
        case il::Op::SlotLoad:
            if (auto cells = facts.shapeSet(x); cells && !cells->empty()) {
                return Licence{LicenceKind::Shape, x, 0, il::Loc::Shapes, std::move(*cells)};
            }
            if (inst.boxType == il::Type::Void) {
                if (const il::Instruction* proto = defs.defInstIs(fn, x, il::Op::ShapeProto);
                    proto != nullptr && proto->operands.size() == 1) {
                    if (const auto chain = protoChainAtShape(module, fn, defs, proto->operands[0], facts);
                        chain && chain->depth + 1 == chain->targetDepth && inst.immI32 == chain->targetSlot) {
                        return Licence{LicenceKind::MemberData, chain->rootShape, static_cast<int64_t>(chain->cell),
                                       il::Loc::Protos};
                    }
                }
            }
            return std::nullopt;
        // A closure's code and captured record never change.
        case il::Op::FnEnv:
        case il::Op::FnConstructs:
            if (const auto callee = facts.calleeOf(x)) return Licence{LicenceKind::Callee, x, *callee, {}};
            return std::nullopt;
        case il::Op::FnInstanceShape:
            if (facts.known(FactKind::FnConstructs, x) == true) {
                return Licence{LicenceKind::FnConstructs, x, 0, il::Loc::FnFields};
            }
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
        case LicenceKind::MemberData:
            return facts.known(FactKind::MemberCell, licence.subject, licence.imm) == true;
        case LicenceKind::Callee:
            return facts.calleeOf(licence.subject) == static_cast<uint32_t>(licence.imm);
        case LicenceKind::FnConstructs:
            return facts.known(FactKind::FnConstructs, licence.subject) == true;
        case LicenceKind::Array:
            return facts.isArray(licence.subject);
        case LicenceKind::TypedArray:
            return facts.isTypedArray(licence.subject);
    }
    return false;
}

}  // namespace bronze::opt::redundancy

#include "codegen-llvm/llvm_repr.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bronze::codegen_llvm {

bool reprCodegenDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_REPR_CODEGEN");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

namespace {

// The JOIN over incoming edges: two claims about the same value agree only
// where they are the same claim, so the lattice is flat above `Unknown` and a
// disagreement falls all the way down. Number and Int32Boxed are NOT joinable
// into "some kind of number" — the whole reason they are separate answers is
// that their BITS differ, and every consumer here reads bits.
ValueRepr join(ValueRepr a, ValueRepr b) {
    if (a == b) return a;
    // A boxed Bool and a boxed Int32 are both NeverPointer, and that much
    // survives a disagreement — it is the fact the GC frame asks for, and it is
    // true of every element of the lattice above `Unknown`.
    if (a != ValueRepr::Unknown && b != ValueRepr::Unknown) return ValueRepr::NotPointer;
    return ValueRepr::Unknown;
}

// What one instruction's RESULT is made of, from the instruction alone.
//
// Everything absent from this list is `Unknown`, which is always sound: it is
// the answer stage R1 gave every value.
ValueRepr defRepr(const il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::Box:
            // `boxType` says what is being boxed; the IL prints it and lowering
            // always sets it, but a Box whose operand type is the authority
            // (`llvm_ops.cpp` reads both) has to answer the same way here or
            // the plan would license a store the emitter does not emit.
            switch (inst.boxType) {
                case il::Type::F64:
                    // The emitter's canonicalizing select: `isnan ? CANONICAL :
                    // bits`. Both arms are at or below NUMBER_MAX, which is
                    // exactly what makes this a Number claim rather than a
                    // "double-ish" one.
                    return ValueRepr::Number;
                case il::Type::I32:
                    return ValueRepr::Int32Boxed;
                case il::Type::Bool:
                    return ValueRepr::NotPointer;
                default:
                    // Str boxes a heap string; anything else reaches
                    // `bronze_box_f64`, whose result is a Number but which
                    // lowering does not otherwise promise, so it stays unknown.
                    return ValueRepr::Unknown;
            }
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
            return ValueRepr::NotPointer;
        default:
            return ValueRepr::Unknown;
    }
}

// The one inference that reads a value's USES rather than its def.
//
// A `dynamic` value every one of whose uses is a RAW unbox is a value the
// compilation has already decided is a Number: `il::Instruction::rawUnbox` is
// granted by `InferenceResult::provenFieldReads` (or by a `--pins` entry the
// write barriers enforce), and what it licenses is a bare bitcast to double
// with no tag test. Spending the same claim on the GC root is not a second
// assumption — a value that bitcasts to a double and is a heap pointer is
// already a miscompile, and the roots are what make it a use-after-move instead
// of a wrong number. The rule is ALL uses, so a value read once raw and once as
// a Value keeps its slot.
//
// `nullishUnbox` gives the weaker half of the same thing: Number, `null` or
// `undefined`, none of which is a pointer.
void applyUseSideProof(const il::Function& func, std::vector<ValueRepr>& repr,
                       const std::vector<bool>& isDynamic) {
    const uint32_t n = func.valueCount;
    // 0 = no use seen, 1 = every use so far is a raw unbox, 2 = every use so far
    // is a raw or nullish unbox, 3 = some other use.
    constexpr uint8_t kUnused = 0;
    constexpr uint8_t kAllRaw = 1;
    constexpr uint8_t kAllNullish = 2;
    constexpr uint8_t kOther = 3;
    std::vector<uint8_t> state(n, kUnused);

    auto note = [&](il::ValueId id, uint8_t kind) {
        if (id == il::kNoValue || id >= n) return;
        state[id] = std::max(state[id], kind);
    };

    for (const auto& block : func.blocks) {
        for (const auto& inst : block.instructions) {
            uint8_t kind = kOther;
            if (inst.op == il::Op::Unbox && inst.type == il::Type::F64) {
                if (inst.rawUnbox) kind = kAllRaw;
                else if (inst.nullishUnbox) kind = kAllNullish;
            }
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                note(inst.operands[i], i == 0 ? kind : kOther);
            }
            for (il::ValueId id : inst.target.args) note(id, kOther);
            for (il::ValueId id : inst.elseTarget.args) note(id, kOther);
        }
    }

    for (uint32_t v = 0; v < n; ++v) {
        if (!isDynamic[v] || repr[v] != ValueRepr::Unknown) continue;
        if (state[v] == kAllRaw) repr[v] = ValueRepr::Number;
        else if (state[v] == kAllNullish) repr[v] = ValueRepr::NotPointer;
    }
}

}  // namespace

ReprPlan planRepr(const il::Function& func) {
    ReprPlan plan;
    const uint32_t n = func.valueCount;
    plan.reprOf.assign(n, ValueRepr::Unknown);
    if (reprCodegenDisabled()) return plan;

    // Which values the IL calls `dynamic`. Only those can carry a GC root, and
    // only those can be the operand of a store site's representation test, so
    // the plan says nothing about the rest — their LLVM type already does.
    std::vector<bool> isDynamic(n, false);
    std::vector<bool> isBlockParam(n, false);
    for (size_t p = 0; p < func.params.size(); ++p) {
        const auto id = static_cast<il::ValueId>(p);
        if (id < n) isDynamic[id] = func.params[p].type == il::Type::Dynamic;
    }
    for (const auto& block : func.blocks) {
        for (const auto& param : block.params) {
            if (param.id == il::kNoValue || param.id >= n) continue;
            isDynamic[param.id] = param.type == il::Type::Dynamic;
            isBlockParam[param.id] = true;
        }
        for (const auto& inst : block.instructions) {
            if (inst.result == il::kNoValue || inst.result >= n) continue;
            isDynamic[inst.result] = inst.type == il::Type::Dynamic;
            plan.reprOf[inst.result] = defRepr(inst);
        }
    }

    applyUseSideProof(func, plan.reprOf, isDynamic);

    // The block parameters, by a fixpoint that starts OPTIMISTIC. A parameter
    // is the join of every argument any edge passes it, and a loop header's
    // parameter is fed by its own body, so the answer cannot be read off in one
    // pass: starting every parameter at the top and lowering it until nothing
    // moves is the standard way, and it terminates because the lattice has four
    // elements and this only ever descends.
    //
    // A parameter with no predecessor at all — an unreachable block — keeps the
    // top, which is harmless: nothing reaches it to spend the claim.
    for (uint32_t v = 0; v < n; ++v) {
        if (isBlockParam[v] && isDynamic[v]) plan.reprOf[v] = ValueRepr::Number;
    }
    auto edgeInto = [&](const il::BlockTarget& target, bool& changed) {
        if (target.block == il::kNoBlock || target.block >= func.blocks.size()) return;
        const auto& params = func.blocks[target.block].params;
        const size_t count = std::min(params.size(), target.args.size());
        for (size_t i = 0; i < count; ++i) {
            const il::ValueId dst = params[i].id;
            if (dst == il::kNoValue || dst >= n || !isDynamic[dst]) continue;
            const il::ValueId src = target.args[i];
            const ValueRepr incoming =
                (src == il::kNoValue || src >= n) ? ValueRepr::Unknown : plan.reprOf[src];
            const ValueRepr next = join(plan.reprOf[dst], incoming);
            if (next != plan.reprOf[dst]) {
                plan.reprOf[dst] = next;
                changed = true;
            }
        }
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& block : func.blocks) {
            for (const auto& inst : block.instructions) {
                edgeInto(inst.target, changed);
                edgeInto(inst.elseTarget, changed);
            }
        }
    }

    for (uint32_t v = 0; v < n; ++v) {
        if (isDynamic[v] && reprNeverPointer(plan.reprOf[v])) ++plan.unrootedValues;
    }
    return plan;
}

ValueRepr storeValueRepr(const il::Function& func, const ReprPlan& plan, size_t blockIndex,
                         size_t instIndex, il::ValueId value) {
    if (reprCodegenDisabled()) return ValueRepr::Unknown;
    const ValueRepr own = plan.at(value);
    if (own == ValueRepr::Number || own == ValueRepr::Int32Boxed) return own;
    if (blockIndex >= func.blocks.size()) return own;
    const auto& insts = func.blocks[blockIndex].instructions;
    if (instIndex == 0 || instIndex > insts.size()) return own;
    // The IMMEDIATELY preceding instruction, and nothing further back. A
    // `pin.guard` proves its value on the path that falls out of it, and the
    // exception check `il::canThrow` puts between the two is what makes that
    // path the only one reaching this store. One instruction of reach is all
    // lowering ever emits (`lower_prop.cpp` emits the guard directly in front of
    // the store), and widening it would mean re-deriving which control edges
    // rejoin in between — a dominance question this plan deliberately does not
    // ask.
    const il::Instruction& prev = insts[instIndex - 1];
    if (prev.op == il::Op::PinGuard && !prev.operands.empty() && prev.operands[0] == value &&
        static_cast<il::PinBarrier>(prev.immI32) == il::PinBarrier::Number) {
        return ValueRepr::Number;
    }
    return own;
}

}  // namespace bronze::codegen_llvm

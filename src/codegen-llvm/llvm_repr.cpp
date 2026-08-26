#include "codegen-llvm/llvm_repr.h"

#include <algorithm>
#include <cstdio>
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

        // NOTHING ELSE, and in particular not `Add`/`Sub`/`Mul`/`Div`/`Mod`
        // over two boxed operands. Their result IS a Number when both operands
        // are -- but only because the emitter's slow arm (the ToPrimitive
        // ladder, string concatenation, the BigInt algorithm) is unreachable
        // there, so the rule would be a claim about what llvm_arith.cpp emits
        // rather than about the IL, and the two would have to be kept in step
        // forever. Measured before it was written: under
        // `BRONZE_REPR_CODEGEN_STATS=1` not one kernel in bench/ has a single
        // such site, because `lower_infer` gives those chains `il::Type::F64`
        // and they never reach a boxed operator at all. The unboxed dataflow
        // stage R2 wanted is the typing that is already there.
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
// assumption -- a value that bitcasts to a double and is a heap pointer is
// already a miscompile, and the roots are what make it a use-after-move instead
// of a wrong number. The rule is ALL uses, so a value read once raw and once as
// a Value keeps its slot.
//
// `nullishUnbox` gives the weaker half of the same thing: Number, `null` or
// `undefined`, none of which is a pointer.
//
// It is a PIN and not a starting point: the values it names have no def rule
// (a property read, a call), so the fixpoint below would lower them straight
// back to `Unknown`.
void collectUseSidePins(const il::Function& func, const std::vector<bool>& isDynamic,
                        std::vector<ValueRepr>& pinned) {
    const uint32_t n = func.valueCount;
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
        if (!isDynamic[v]) continue;
        if (state[v] == kAllRaw) pinned[v] = ValueRepr::Number;
        else if (state[v] == kAllNullish) pinned[v] = ValueRepr::NotPointer;
    }
}

}  // namespace

ReprStats& reprStats() {
    static ReprStats stats;
    return stats;
}

void reprStatsReport() {
    const char* env = std::getenv("BRONZE_REPR_CODEGEN_STATS");
    if (env == nullptr || std::strcmp(env, "1") != 0) return;
    const ReprStats& s = reprStats();
    std::fprintf(stderr,
                 "[repr] functions=%u provenNumber=%u rootsElided=%u rawStores=%u "
                 "sitofpStores=%u\n",
                 s.functions, s.provenNumber, s.rootsElided, s.rawStores, s.sitofpStores);
}

ReprPlan planRepr(const il::Function& func) {
    ReprPlan plan;
    const uint32_t n = func.valueCount;
    plan.reprOf.assign(n, ValueRepr::Unknown);
    if (reprCodegenDisabled()) return plan;

    // Which values the IL calls `dynamic`. Only those can carry a GC root, and
    // only those can be the operand of a store site's representation test, so
    // the plan says nothing about the rest -- their LLVM type already does.
    std::vector<bool> isDynamic(n, false);
    for (size_t p = 0; p < func.params.size(); ++p) {
        const auto id = static_cast<il::ValueId>(p);
        if (id < n) isDynamic[id] = func.params[p].type == il::Type::Dynamic;
    }
    for (const auto& block : func.blocks) {
        for (const auto& param : block.params) {
            if (param.id == il::kNoValue || param.id >= n) continue;
            isDynamic[param.id] = param.type == il::Type::Dynamic;
        }
        for (const auto& inst : block.instructions) {
            if (inst.result == il::kNoValue || inst.result >= n) continue;
            isDynamic[inst.result] = inst.type == il::Type::Dynamic;
        }
    }

    std::vector<ValueRepr> pinned(n, ValueRepr::Unknown);
    collectUseSidePins(func, isDynamic, pinned);

    // OPTIMISTIC, and therefore monotone in ONE direction. Every value starts
    // ABOVE the lattice and the rules only ever lower it, which is what makes
    // the whole thing terminate -- and it is also what makes a loop-carried
    // value answerable at all: a block parameter is defined in terms of the
    // edges into it, one of which comes from inside the loop and is defined in
    // terms of the parameter. Only a greatest fixpoint says the true thing
    // about that circle; a forward walk would have to guess.
    //
    // `kTop` is a FIFTH state and not `Number`, because meeting a fresh value
    // with the rule's answer has to GIVE the rule's answer. Number and
    // Int32Boxed disagree, so starting at Number would turn the first Int32 box
    // it met into their disagreement (NotPointer) and quietly lose the one fact
    // the inline conversion is spent on.
    constexpr uint8_t kTop = 0xFF;
    std::vector<uint8_t> cur(n, kTop);

    // A pinned value is SEEDED and never lowered: `collectUseSidePins` read the
    // value's uses, and no def rule speaks for the property reads and calls it
    // names.
    for (uint32_t v = 0; v < n; ++v) {
        if (pinned[v] != ValueRepr::Unknown) cur[v] = static_cast<uint8_t>(pinned[v]);
    }
    // A FUNCTION parameter has no rule and no incoming edge, so nothing below
    // would ever lower it off the top.
    for (size_t p = 0; p < func.params.size(); ++p) {
        const auto id = static_cast<il::ValueId>(p);
        if (id < n && pinned[id] == ValueRepr::Unknown) cur[id] = static_cast<uint8_t>(ValueRepr::Unknown);
    }

    auto meet = [&](uint8_t a, ValueRepr b) -> uint8_t {
        if (a == kTop) return static_cast<uint8_t>(b);
        return static_cast<uint8_t>(join(static_cast<ValueRepr>(a), b));
    };
    auto lower = [&](il::ValueId id, ValueRepr to, bool& changed) {
        if (id == il::kNoValue || id >= n) return;
        if (pinned[id] != ValueRepr::Unknown) return;
        const uint8_t next = meet(cur[id], to);
        if (next != cur[id]) {
            cur[id] = next;
            changed = true;
        }
    };
    auto at = [&](il::ValueId id) {
        if (id == il::kNoValue || id >= n || cur[id] == kTop) return ValueRepr::Unknown;
        return static_cast<ValueRepr>(cur[id]);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& block : func.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.result != il::kNoValue && inst.result < n) {
                    lower(inst.result, defRepr(inst), changed);
                }
                for (const il::BlockTarget* target : {&inst.target, &inst.elseTarget}) {
                    if (target->block == il::kNoBlock || target->block >= func.blocks.size()) {
                        continue;
                    }
                    const auto& params = func.blocks[target->block].params;
                    const size_t count = std::min(params.size(), target->args.size());
                    for (size_t i = 0; i < count; ++i) {
                        lower(params[i].id, at(target->args[i]), changed);
                    }
                }
            }
        }
    }
    // Anything still at the top is a value nothing reaches -- an unreferenced
    // block parameter, a dead def -- and `Unknown` is the answer that costs
    // nothing to be wrong about.
    for (uint32_t v = 0; v < n; ++v) plan.reprOf[v] = at(static_cast<il::ValueId>(v));

    // A value the IL does not call `dynamic` has an LLVM type that already says
    // everything, and letting the optimistic start survive on one would let a
    // caller read a claim about, say, an `i32` register. Cleared here rather
    // than special-cased at every rule.
    for (uint32_t v = 0; v < n; ++v) {
        if (!isDynamic[v]) plan.reprOf[v] = ValueRepr::Unknown;
        else if (reprNeverPointer(plan.reprOf[v])) ++plan.unrootedValues;
    }

    ReprStats& stats = reprStats();
    ++stats.functions;
    stats.rootsElided += plan.unrootedValues;
    for (uint32_t v = 0; v < n; ++v) {
        if (reprIsNumber(plan.reprOf[v])) ++stats.provenNumber;
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

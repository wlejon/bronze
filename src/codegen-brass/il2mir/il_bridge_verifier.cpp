#include "il_bridge_verifier.h"
#include "il_abi.h"
#include <brass/core/parallel_for.hpp>
#include <brass/mir/verifier.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace il2mir {

namespace {

constexpr uint64_t kTagMaskBits = ~BRONZE_ABI_VALUE_PAYLOAD_MASK;

bool is_const_i64(const brass::Value* v, uint64_t expected) {
    if (!v || !v->is_instruction()) return false;
    const brass::Instruction* def = v->defining_instruction();
    if (!def || def->opcode() != brass::Opcode::iconst_i64) return false;
    return static_cast<uint64_t>(def->imm_i64()) == expected;
}

bool is_const_i64_any(const brass::Value* v, uint64_t& out) {
    if (!v || !v->is_instruction()) return false;
    const brass::Instruction* def = v->defining_instruction();
    if (!def || def->opcode() != brass::Opcode::iconst_i64) return false;
    out = static_cast<uint64_t>(def->imm_i64());
    return true;
}

// The edges into each block of a function, in block order (each block's
// last instruction: its branch, true, false and switch-case targets), built
// on first use: a block parameter's incoming arguments are asked of it once
// per parameter instead of by a walk of every block.
class IncomingEdges {
public:
    explicit IncomingEdges(const brass::Function& fn) : fn_(fn) {}

    const std::vector<const brass::BranchTarget*>& into(const brass::BasicBlock* bb) {
        if (!built_) build();
        auto it = edges_.find(bb);
        return it == edges_.end() ? none_ : it->second;
    }

private:
    void build() {
        built_ = true;
        for (const brass::BasicBlock* bb : fn_.blocks()) {
            const brass::Instruction* term = bb ? bb->tail() : nullptr;
            if (!term) continue;
            auto add = [&](const brass::BranchTarget& t) {
                if (t.block) edges_[t.block].push_back(&t);
            };
            add(term->branch_target());
            add(term->true_target());
            add(term->false_target());
            for (const auto& sc : term->switch_cases()) add(sc.target);
        }
    }

    const brass::Function& fn_;
    bool built_ = false;
    std::unordered_map<const brass::BasicBlock*, std::vector<const brass::BranchTarget*>> edges_;
    const std::vector<const brass::BranchTarget*> none_;
};

// A double that IS a Value's bits rather than the result of arithmetic: a
// boxed Array element or a double slot loaded as one (il_abi.h
// kBoxedElementAliasClass, kObjectSlotAliasClass), an
// i64's bits reinterpreted, or a block parameter every incoming edge
// passes one of those (the read runs of il_lowering_hoist.cpp merge them).
// `seen` cuts a loop of parameters: a cycle contributes no other value.
bool is_value_bits_f64(IncomingEdges& incoming, const brass::Value* v,
                       std::unordered_set<const brass::Value*>& seen) {
    if (!v) return false;
    if (v->is_block_param()) {
        if (!seen.insert(v).second) return true;
        bool any = false;
        for (const brass::BranchTarget* t : incoming.into(v->defining_block())) {
            if (v->param_index() >= t->args.size()) continue;
            any = true;
            if (!is_value_bits_f64(incoming, t->args[v->param_index()], seen)) return false;
        }
        return any;
    }
    const brass::Instruction* def = v->is_instruction() ? v->defining_instruction() : nullptr;
    if (!def) return false;
    if ((def->opcode() == brass::Opcode::load || def->opcode() == brass::Opcode::load_indexed) &&
        (def->alias_class() == kBoxedElementAliasClass || def->alias_class() == kObjectSlotAliasClass)) {
        return true;
    }
    // An i64 reinterpreted: turned back, it is the same i64, which this rule
    // has nothing to say about (it is about doubles that arithmetic made).
    return def->opcode() == brass::Opcode::bitcast_f64_i64;
}

bool bits_need_no_canonicalization(IncomingEdges& incoming, const brass::Value* v) {
    std::unordered_set<const brass::Value*> seen;
    if (is_value_bits_f64(incoming, v, seen)) return true;
    if (!v || !v->is_instruction()) return false;
    const brass::Instruction* def = v->defining_instruction();
    if (!def) return false;
    switch (def->opcode()) {
        case brass::Opcode::sitofp_f64_i32:
        case brass::Opcode::sitofp_f64_i64:
            return true;
        case brass::Opcode::fconst_f64: {
            double d = def->imm_f64();
            if (!std::isnan(d)) return true;
            uint64_t bits = 0;
            std::memcpy(&bits, &d, sizeof(bits));
            return bits == BRONZE_ABI_CANONICAL_NAN_BITS;
        }
        default:
            return false;
    }
}

bool is_sign_mask_of(const brass::Value* v, const brass::Value* target) {
    if (!v || !v->is_instruction()) return false;
    const brass::Instruction* def = v->defining_instruction();
    if (!def || def->opcode() != brass::Opcode::and_ || def->operand_count() != 2) return false;
    const brass::Value* a = def->operand(0);
    const brass::Value* b = def->operand(1);
    return (a == target && is_const_i64(b, 0x7FFFFFFFFFFFFFFFULL)) ||
           (b == target && is_const_i64(a, 0x7FFFFFFFFFFFFFFFULL));
}

bool is_canonical_nan_guard_cond(const brass::Value* cond, const brass::Value* raw_bits) {
    if (!cond || !cond->is_instruction()) return false;
    const brass::Instruction* def = cond->defining_instruction();
    if (!def || def->operand_count() != 2) return false;
    if (def->opcode() == brass::Opcode::ugt) {
        return is_sign_mask_of(def->operand(0), raw_bits) &&
               is_const_i64(def->operand(1), 0x7FF0000000000000ULL);
    }
    if (def->opcode() == brass::Opcode::ult) {
        return is_const_i64(def->operand(0), 0x7FF0000000000000ULL) &&
               is_sign_mask_of(def->operand(1), raw_bits);
    }
    // `f != f` on the double the bits are (box_f64_bits): true exactly for a NaN.
    if (def->opcode() == brass::Opcode::ne && def->operand(0) == def->operand(1)) {
        const brass::Instruction* bits_def = raw_bits->defining_instruction();
        return bits_def && bits_def->opcode() == brass::Opcode::bitcast_i64_f64 &&
               bits_def->operand(0) == def->operand(0) && def->operand(0)->type() == brass::Type::f64();
    }
    return false;
}

bool is_canonical_nan_select_use(const brass::Instruction* user, const brass::Value* raw_bits) {
    if (!user || user->opcode() != brass::Opcode::select || user->operand_count() != 3) {
        return false;
    }
    return is_canonical_nan_guard_cond(user->operand(0), raw_bits) &&
           is_const_i64(user->operand(1), BRONZE_ABI_CANONICAL_NAN_BITS) &&
           user->operand(2) == raw_bits;
}

bool is_sign_mask_use(const brass::Instruction* user, const brass::Value* raw_bits) {
    if (!user || user->opcode() != brass::Opcode::and_ || user->operand_count() != 2) {
        return false;
    }
    return (user->operand(0) == raw_bits && is_const_i64(user->operand(1), 0x7FFFFFFFFFFFFFFFULL)) ||
           (user->operand(1) == raw_bits && is_const_i64(user->operand(0), 0x7FFFFFFFFFFFFFFFULL));
}

// An equality test of the bits against a constant (is.i32's -0 test): it reads
// them, and no NaN in them reaches a box.
bool is_const_compare_use(const brass::Instruction* user, const brass::Value* raw_bits) {
    if (!user || (user->opcode() != brass::Opcode::eq && user->opcode() != brass::Opcode::ne) ||
        user->operand_count() != 2) {
        return false;
    }
    uint64_t c = 0;
    return (user->operand(0) == raw_bits && is_const_i64_any(user->operand(1), c)) ||
           (user->operand(1) == raw_bits && is_const_i64_any(user->operand(0), c));
}

bool is_nan_box_boundary_const(uint64_t c) {
    if (c == kBronzeNumberMaxBits || c == 0x7FF0000000000000ULL || c == 0x43E0000000000000ULL) {
        return true;
    }
    return (c & kTagMaskBits) >= kBronzeNumberMaxBits && (c & ~kTagMaskBits) == 0;
}

bool is_valid_shifted_tag(uint64_t c) {
    if ((c & ~kTagMaskBits) != 0) return false;
    uint64_t tag = c >> BRONZE_ABI_VALUE_TAG_SHIFT;
    return tag >= 0xFFF0 && tag <= 0xFFFF;
}

bool is_tag_masked_value(const brass::Value* v) {
    if (!v || !v->is_instruction()) return false;
    const brass::Instruction* def = v->defining_instruction();
    if (!def || def->opcode() != brass::Opcode::and_ || def->operand_count() != 2) return false;
    return is_const_i64(def->operand(0), kTagMaskBits) || is_const_i64(def->operand(1), kTagMaskBits);
}

bool branch_targets_use_value(const brass::Instruction* inst, const brass::Value* v) {
    for (const brass::Value* arg : inst->branch_target().args) {
        if (arg == v) return true;
    }
    for (const brass::Value* arg : inst->true_target().args) {
        if (arg == v) return true;
    }
    for (const brass::Value* arg : inst->false_target().args) {
        if (arg == v) return true;
    }
    for (const auto& sc : inst->switch_cases()) {
        for (const brass::Value* arg : sc.target.args) {
            if (arg == v) return true;
        }
    }
    return false;
}

// The bridge's own checks of one function (everything but the structural
// MIR verification).
BridgeVerifyResult verify_bridge_rules(const brass::Function& fn) {
    BridgeVerifyResult res;
    IncomingEdges incoming(fn);
    std::vector<std::pair<uint32_t, const brass::Value*>> uncanonicalized_f64_bits;

    for (const brass::BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (const brass::Instruction* inst = bb->head(); inst != nullptr; inst = inst->next()) {
            const brass::Opcode op = inst->opcode();

            // 1. Track bitcast_i64_f64 instructions whose input is not proven non-NaN.
            if (op == brass::Opcode::bitcast_i64_f64 && inst->operand_count() == 1 && inst->result()) {
                if (!bits_need_no_canonicalization(incoming, inst->operand(0))) {
                    uncanonicalized_f64_bits.emplace_back(bb->id(), inst->result());
                }
            }
            // 2. Check signed ordering comparisons against NaN-box boundary constants.
            if (op == brass::Opcode::slt || op == brass::Opcode::sle ||
                op == brass::Opcode::sgt || op == brass::Opcode::sge) {
                uint64_t c0 = 0, c1 = 0;
                bool has0 = inst->operand_count() >= 1 && is_const_i64_any(inst->operand(0), c0);
                bool has1 = inst->operand_count() >= 2 && is_const_i64_any(inst->operand(1), c1);
                if ((has0 && is_nan_box_boundary_const(c0)) || (has1 && is_nan_box_boundary_const(c1))) {
                    res.ok = false;
                    res.errors.push_back({
                        std::string(fn.name()),
                        bb->id(),
                        "Signed comparison used against NaN-box tag/range boundary constant; must use unsigned comparison (ult/ule/ugt/uge)"
                    });
                }
            }

            // 3. Check tag-masked equality comparisons.
            if ((op == brass::Opcode::eq || op == brass::Opcode::ne) && inst->operand_count() == 2) {
                uint64_t c = 0;
                if (is_tag_masked_value(inst->operand(0)) && is_const_i64_any(inst->operand(1), c)) {
                    if (!is_valid_shifted_tag(c)) {
                        res.ok = false;
                        res.errors.push_back({
                            std::string(fn.name()),
                            bb->id(),
                            "Tag-masked value (v & kTagMask) compared against invalid shifted tag constant"
                        });
                    }
                } else if (is_tag_masked_value(inst->operand(1)) && is_const_i64_any(inst->operand(0), c)) {
                    if (!is_valid_shifted_tag(c)) {
                        res.ok = false;
                        res.errors.push_back({
                            std::string(fn.name()),
                            bb->id(),
                            "Tag-masked value (v & kTagMask) compared against invalid shifted tag constant"
                        });
                    }
                }
            }
        }
    }

    // Verify that every uncanonicalized bitcast_i64_f64 result is only consumed by
    // sign-masking (& 0x7FFFFFFFFFFFFFFF) or canonical-NaN selection. Their
    // readers are found in one walk of the function, in its order.
    if (!uncanonicalized_f64_bits.empty()) {
        std::unordered_map<const brass::Value*, size_t> which;
        for (size_t i = 0; i < uncanonicalized_f64_bits.size(); ++i) {
            which.emplace(uncanonicalized_f64_bits[i].second, i);
        }
        std::vector<std::vector<const brass::Instruction*>> readers(uncanonicalized_f64_bits.size());
        for (const brass::BasicBlock* bb : fn.blocks()) {
            if (!bb) continue;
            for (const brass::Instruction* inst = bb->head(); inst != nullptr; inst = inst->next()) {
                auto note = [&](const brass::Value* v) {
                    auto it = which.find(v);
                    if (it == which.end()) return;
                    auto& r = readers[it->second];
                    if (r.empty() || r.back() != inst) r.push_back(inst);
                };
                for (const brass::Value* opd : inst->operands()) note(opd);
                for (const brass::Value* arg : inst->branch_target().args) note(arg);
                for (const brass::Value* arg : inst->true_target().args) note(arg);
                for (const brass::Value* arg : inst->false_target().args) note(arg);
                for (const auto& sc : inst->switch_cases()) {
                    for (const brass::Value* arg : sc.target.args) note(arg);
                }
            }
        }
        for (size_t i = 0; i < uncanonicalized_f64_bits.size(); ++i) {
            const auto& [def_block_id, raw_bits] = uncanonicalized_f64_bits[i];
            for (const brass::Instruction* inst : readers[i]) {
                bool uses_raw = false;
                for (const brass::Value* opd : inst->operands()) {
                    if (opd == raw_bits) {
                        uses_raw = true;
                        break;
                    }
                }
                if (uses_raw) {
                    if (!is_sign_mask_use(inst, raw_bits) && !is_const_compare_use(inst, raw_bits) &&
                        !is_canonical_nan_select_use(inst, raw_bits)) {
                        res.ok = false;
                        res.errors.push_back({
                            std::string(fn.name()),
                            def_block_id,
                            "Uncanonicalized bitcast_i64_f64 escapes without NaN canonicalization; negative NaNs (0xFFF8...) would corrupt NaN-box tags"
                        });
                    }
                }
                if (branch_targets_use_value(inst, raw_bits)) {
                    res.ok = false;
                    res.errors.push_back({
                        std::string(fn.name()),
                        def_block_id,
                        "Uncanonicalized bitcast_i64_f64 passed as block argument without NaN canonicalization"
                    });
                }
            }
        }
    }

    return res;
}

} // namespace

BridgeVerifyResult verify_bridge_function_detailed(const brass::Function& fn) {
    BridgeVerifyResult res;
    if (!fn.body_ready() || fn.blocks().empty()) {
        return res;
    }

    brass::DiagnosticReporter mir_diag;
    if (!brass::verify_function(fn, &mir_diag)) {
        res.ok = false;
        res.errors.push_back({std::string(fn.name()), 0, "MIR verification failed: " + mir_diag.format_all()});
    }
    BridgeVerifyResult rules = verify_bridge_rules(fn);
    if (!rules.ok) {
        res.ok = false;
        res.errors.insert(res.errors.end(), rules.errors.begin(), rules.errors.end());
    }
    return res;
}

BridgeVerifyResult verify_bridge_module_detailed(const brass::Module& mod, size_t threads) {
    BridgeVerifyResult res;
    brass::DiagnosticReporter mir_diag;
    if (!brass::verify_module(mod, &mir_diag, threads)) {
        res.ok = false;
        res.errors.push_back({"<module>", 0, "MIR module verification failed: " + mir_diag.format_all()});
    }
    // Each function checked on its own (the checks only read it), the
    // errors reported in module order.
    const std::vector<brass::Function*>& fns = mod.functions();
    std::vector<size_t> weights(fns.size(), 1);
    size_t total = 0;
    for (size_t i = 0; i < fns.size(); ++i) {
        if (fns[i]) weights[i] += fns[i]->block_count();
        total += weights[i];
    }
    std::vector<BridgeVerifyResult> per(fns.size());
    const size_t n = brass::threads_for_work(threads, fns.size(), total, 4000);
    brass::parallel_for_each(brass::heaviest_first(weights), n, [&](size_t i) {
        const brass::Function* fn = fns[i];
        if (!fn || !fn->body_ready() || fn->blocks().empty()) return;
        // The structural verification ran over the module above.
        per[i] = verify_bridge_rules(*fn);
    });
    for (BridgeVerifyResult& fn_res : per) {
        if (fn_res.ok) continue;
        res.ok = false;
        res.errors.insert(res.errors.end(), fn_res.errors.begin(), fn_res.errors.end());
    }
    return res;
}

bool verify_bridge_function(const brass::Function& fn, brass::DiagnosticReporter* diag) {
    BridgeVerifyResult res = verify_bridge_function_detailed(fn);
    if (!res.ok && diag) {
        for (const auto& err : res.errors) {
            diag->error(brass::SourceLocation(err.function_name, err.block_id, 0), err.message);
        }
    }
    return res.ok;
}

bool verify_bridge_module(const brass::Module& mod, brass::DiagnosticReporter* diag, size_t threads) {
    BridgeVerifyResult res = verify_bridge_module_detailed(mod, threads);
    if (!res.ok && diag) {
        for (const auto& err : res.errors) {
            diag->error(brass::SourceLocation(err.function_name, err.block_id, 0), err.message);
        }
    }
    return res.ok;
}

} // namespace il2mir

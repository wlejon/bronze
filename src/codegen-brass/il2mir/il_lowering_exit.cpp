// A speculated function's `leave` as a brass guard into G's resume twin
// (brass/mir/resume_twin.hpp, "Guards in AOT images").
//
// The speculation step writes a guard as an ordinary branch to a cold exit
// block that boxes what S holds unboxed and ends in `leave @G, id, state`.
// The block stays a cold block, so every optimization of branches (threading,
// and-chains split into one branch each) applies to the branch into it, and
// the leave at its end is a guard that always fails, `guard 0, @G__twin,
// [state]`, then `unreachable`. brass's guard exit reads each state value
// from wherever it lives there and calls twin(id, &state) (emit_guard_exit),
// and its register allocator weighs those reads as needing no register: a
// value only exits read is spilled once where it is made and costs the
// guarded path nothing more, where stores of it into a buffer for a call
// made it as dear to keep as a value the guarded code reads. A box only the
// guard reads (a double's canonical bits, an int32's double) is made by the
// exit from the unboxed value (brass x64_isel_guard_state.cpp).
//
// The state is the leave's operands as S holds them, then the hidden values
// (il_lowering_resume.cpp). Whether G holds a dynamic value tagged or as its
// bits is known only once G is lowered, so match_twin_state_types converts
// each value to the type its twin reads after every body exists: a bitcast
// between the two forms of a Value's bits, or the box of a double.
//
// A leave whose G returns another type than S (the twin's result is S's
// result here), and a coroutine body's, keep the stores and the call of
// lower_leave_call.

#include "il_lowering.h"
#include "il_lowering_calls.h"
#include "il_lowering_ops.h"

#include <brass/mir/resume_twin.hpp>

#include <string>
#include <vector>

namespace il2mir {

namespace {

// `v` as the twin reads it, `want`: the same bits in the type G holds them
// in. Null when no bit-preserving conversion exists.
Value* twin_word(Builder& b, Value* v, Type want) {
    const Type have = v->type();
    if (have == want) return v;
    if (have == Type::f64()) {
        if (want == Type::i64()) return box_f64_bits(b, v);
        if (want.is_tagged()) return b.build_bitcast_tagged_i64(box_f64_bits(b, v));
        return nullptr;
    }
    Value* bits = nullptr;
    if (have.is_tagged()) {
        bits = b.build_bitcast_i64_tagged(v);
    } else if (have == Type::i64()) {
        bits = v;
    } else if (have == Type::i32() || have == Type::i8()) {
        // A Boolean's 0/1 or an int32, as the leave's words held them.
        if (want == Type::i64()) return b.build_zext_i64(v);
        if (want == Type::i32()) return b.build_trunc_i32(b.build_zext_i64(v));
        if (want == Type::i8()) return b.build_trunc_i8(v);
        return nullptr;
    }
    if (bits == nullptr) return nullptr;
    if (want == Type::i64()) return bits;
    if (want.is_tagged()) return b.build_bitcast_tagged_i64(bits);
    if (want == Type::f64()) return b.build_bitcast_f64_i64(bits);
    if (want == Type::i32()) return b.build_trunc_i32(bits);
    return nullptr;
}

}  // namespace

const Function* IlLowering::leave_target(const BronzeInstruction& inst, const Function& fn) const {
    if (current_ast_ == nullptr || inst.index >= current_ast_->functions.size() ||
        inst.index >= resolved_names_.size() || resolved_names_[inst.index].empty()) {
        return nullptr;
    }
    // A coroutine body's exits stay calls: brass splits its frame around them.
    const BronzeFunction* s_ast = current_fn_ast();
    if (s_ast == nullptr || s_ast->is_coroutine_body()) return nullptr;
    const Function* g = fn.parent()->get_function(resolved_names_[inst.index]);
    return g != nullptr && g->return_type() == fn.return_type() ? g : nullptr;
}

bool IlLowering::lower_leave(const BronzeInstruction& inst, Builder& b, Function* fn,
                             std::unordered_map<uint32_t, Value*>& val_map) {
    // An exit: the function's speculation failed, and the rest of the call is
    // the twin's. Laid out after the main line.
    b.current_block()->set_cold();
    const Function* g = leave_target(inst, *fn);
    if (g == nullptr) return lower_leave_call(inst, b, fn, val_map);
    const BronzeFunction& g_ast = current_ast_->functions[inst.index];
    const auto& types = il_types();
    std::vector<Value*> state;
    state.reserve(inst.operands.size() + 2);
    for (uint32_t opd : inst.operands) {
        Value* v = get_val_by_id(opd, b, val_map);
        const auto ty = types.find(opd);
        if (v == nullptr || ty == types.end()) {
            return inst_error(inst, "leave state value %" + std::to_string(opd) + " is not defined here");
        }
        switch (ty->second) {
            case BronzeType::F64: v = ensure_type(v, Type::f64(), b); break;
            case BronzeType::I32: v = ensure_type(v, Type::i32(), b); break;
            case BronzeType::Bool:
                v = ensure_type(v, Type::i32(), b);
                if (!is_zero_one(v)) v = b.build_and(v, b.build_iconst_i32(1));
                break;
            default: break;
        }
        state.push_back(v);
    }
    if (options_.per_thread_module_data) state.push_back(module_delta(b));
    if (argv_block_words(this, g_ast) > 0) {
        if (argv_block_ == nullptr) return inst_error(inst, "leave without an argv block for its twin");
        state.push_back(argv_block_);
    }
    Instruction* guard = b.build_guard(b.build_iconst_i32(0), brass::resume_twin_name(g->name()),
                                       Span<Value* const>(state.data(), state.size()));
    guard->set_resume_id(static_cast<uint32_t>(inst.imm_i64));
    b.build_unreachable();
    return true;
}

bool IlLowering::match_twin_state_types(Module& mod) {
    Builder b(mod);
    for (Function* fn : mod.functions()) {
        if (fn == nullptr || fn->is_resume_twin()) continue;
        b.set_function(fn);
        for (BasicBlock* bb : fn->blocks()) {
            if (bb == nullptr) continue;
            for (Instruction* inst = bb->head(); inst != nullptr; inst = inst->next()) {
                if (inst->opcode() != Opcode::guard) continue;
                const Function* twin = fn->guard_exit_stub(*inst);
                if (twin == nullptr || !twin->is_resume_twin()) continue;
                const std::vector<Type>* want = twin->twin_state_types(inst->resume_id());
                auto& state = inst->state_map();
                if (want == nullptr || want->size() != state.size()) {
                    has_error_ = true;
                    if (diag_) {
                        diag_->error(SourceLocation("", 0, 0),
                                     "il2mir: " + std::string(fn->name()) + " leaves to " +
                                         std::string(twin->name()) + " at resume point " +
                                         std::to_string(inst->resume_id()) + " with a state it does not read");
                    }
                    return false;
                }
                b.position_before(inst);
                for (size_t i = 0; i < state.size(); ++i) {
                    if (state[i] == nullptr || state[i]->type() == (*want)[i]) continue;
                    Value* w = twin_word(b, state[i], (*want)[i]);
                    if (w == nullptr) {
                        has_error_ = true;
                        if (diag_) {
                            diag_->error(SourceLocation("", 0, 0),
                                         "il2mir: " + std::string(fn->name()) + " holds state value " +
                                             std::to_string(i) + " of resume point " +
                                             std::to_string(inst->resume_id()) + " as " +
                                             std::string(state[i]->type().name()) + " where " +
                                             std::string(twin->name()) + " reads " +
                                             std::string((*want)[i].name()));
                        }
                        return false;
                    }
                    state[i] = w;
                }
            }
        }
    }
    return true;
}

}  // namespace il2mir

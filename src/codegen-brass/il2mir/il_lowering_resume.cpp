// Leaving a speculated function (il_op.h `resume.point` / `leave`).
//
// A generic function G with resume points gets a RESUME TWIN, brass's
// `G__twin(i32 id, ptr state)` (brass/mir/resume_twin.hpp): G's code with
// each point as an entry, which reads the point's state from `state`, eight
// bytes a value in state order, and runs G from there to its return. A
// speculated function S leaves by calling it: its `leave @G, id, ...` stores
// the state into a buffer of its frame and returns what the twin returns.
// An INLINED speculated body (opt/inline) finishes its call the same way with
// `resume.call @G, id, ...`, which stores the same state into the same buffer
// and calls the same twin, but takes its return value as a value and goes on.
// Nothing of this is in G itself: lowered, each point is a guard that never
// fails (brass's site form, which is what build_resume_twin reads) in front
// of a block of its own, and once the twin is cut from G the guards and their
// resume table go, leaving G a straight branch where each point was.
//
// THE STATE is the point's IL operands, in order, and then the HIDDEN VALUES:
// the values G's lowering computes once at its entry and reads further on,
// which the IL does not see. The twin reads only what the state holds, so
// every one has to be there, and in the same order on both sides:
//   1. the module delta (TranslatorOptions::per_thread_module_data), which
//      every per-thread table address adds;
//   2. the argv block (the function's alloca.tagged a call stages its
//      arguments in), when G's lowering has one: S hands its own, which is
//      sized for G's calls too (lower_function), and which lives for as long
//      as the twin runs, under S's frame.
// Nothing else crosses a resume point: the IC table's address is computed
// per use (il_lowering_hoist.cpp), every look-through of the lowering's
// peepholes stops at the current block (ensure_type, unbox_tagged, the Box
// and Unbox of il_lowering_inst.cpp, lower_to_int32), the 0/1 values noted
// for branch negation are forgotten at each point, a module env read loads
// afresh, the pinned TLS block is a register, and the coroutine frame and a
// handler's exception never meet a point (il/resume_rules.h). brass's verifier
// checks the rest where it runs: a resume target's live-ins must all be in
// its guard's state (verify_guard_resume_state).
//
// The buffer S fills is an alloca.tagged: between S's stores and the twin's
// loads the twin's prologue may service an interrupt and collect, and the
// buffer's dynamic words must move with what they name. So every word is a
// valid Value: a dynamic value's bits, an i32 or a boolean zero-extended (a
// small double to the collector), a double canonicalized (box_f64_bits: any
// NaN reads back as the NaN it is), a raw word (a shape's address, below
// 2^48) as itself.

#include "il_abi.h"
#include "il_lowering.h"
#include "il_lowering_calls.h"
#include "il_lowering_ops.h"

#include <brass/mir/resume_twin.hpp>

#include <string>
#include <vector>

namespace il2mir {

const std::unordered_map<uint32_t, BronzeType>& IlLowering::il_types() {
    if (il_types_built_) return il_types_;
    il_types_built_ = true;
    il_types_.clear();
    const BronzeFunction* fn_ast = current_fn_ast();
    if (!fn_ast) return il_types_;
    for (const auto& [id, t] : fn_ast->params) il_types_.emplace(id, t);
    for (const BronzeBlock& blk : fn_ast->blocks) {
        for (const auto& [id, t] : blk.params) il_types_.emplace(id, t);
        for (const BronzeInstruction& inst : blk.instructions) {
            if (inst.result_id != UINT32_MAX) il_types_.emplace(inst.result_id, inst.result_type);
        }
    }
    return il_types_;
}

namespace {

// Whether `t`, a resume point's state value of IL type `il`, has the MIR type
// a leave's word (store_state_word) reads back as.
bool state_type_matches(BronzeType il, Type t) {
    switch (il) {
        case BronzeType::Dynamic:
        case BronzeType::Unknown: return t.is_tagged();
        case BronzeType::F64: return t == Type::f64();
        case BronzeType::I32: return t == Type::i32();
        case BronzeType::Bool: return t == Type::i32() || t == Type::i8();
        case BronzeType::Ptr: return t == Type::i64();
        case BronzeType::Str: return t == Type::ptr() || t == Type::i64();
        case BronzeType::Void: return false;
    }
    return false;
}

}  // namespace

bool IlLowering::inst_error(const BronzeInstruction& inst, const std::string& what) {
    has_error_ = true;
    if (diag_) diag_->error(SourceLocation("", inst.line, inst.column), "il2mir: " + what);
    return false;
}

bool IlLowering::lower_resume_point(const BronzeInstruction& inst, Builder& b, Function* fn,
                                    const std::unordered_map<uint32_t, Value*>& val_map) {
    const BronzeFunction* fn_ast = current_fn_ast();
    if (fn_ast == nullptr || fn_ast->is_coroutine_body() || coro_frame_val_ != nullptr) {
        return inst_error(inst, "resume.point in a coroutine body");
    }
    const auto id = static_cast<uint32_t>(inst.imm_i64);
    if (fn->get_resume_target(id) != nullptr) {
        return inst_error(inst, "resume.point " + std::to_string(id) + " twice in one function");
    }
    const auto& types = il_types();
    std::vector<Value*> state;
    state.reserve(inst.operands.size() + 2);
    for (uint32_t opd : inst.operands) {
        // The value as held, not read: a dynamic value stays tagged (a root),
        // and the region reads it under this very name.
        const auto it = val_map.find(opd);
        const auto ty = types.find(opd);
        if (it == val_map.end() || it->second == nullptr || ty == types.end()) {
            return inst_error(inst, "resume.point state value %" + std::to_string(opd) + " is not defined here");
        }
        if (!state_type_matches(ty->second, it->second->type())) {
            return inst_error(inst, "resume.point state value %" + std::to_string(opd) +
                                          " is held in a representation a leave does not write");
        }
        state.push_back(it->second);
    }
    // THE HIDDEN VALUES, in their fixed order (above).
    if (options_.per_thread_module_data) {
        if (current_module_delta_ == nullptr) return inst_error(inst, "resume.point before the module delta");
        state.push_back(current_module_delta_);
    }
    if (argv_block_ != nullptr) state.push_back(argv_block_);

    BasicBlock* cur = b.current_block();
    BasicBlock* cont = b.append_block("resume" + std::to_string(id));
    if (cur->is_cold()) cont->set_cold();
    b.position_at_end(cur);
    Instruction* guard = b.build_guard(b.build_iconst_i32(1), "brass.spec",
                                       Span<Value* const>(state.data(), state.size()));
    guard->set_resume_id(id);
    fn->add_resume_point(id, cont);
    b.build_br(cont);
    b.position_at_end(cont);
    // A 0/1 value noted before the point is not in its state.
    boolean_values_.clear();
    return true;
}

Function* IlLowering::stage_twin_state(const BronzeInstruction& inst, Builder& b, Function* fn,
                                       std::unordered_map<uint32_t, Value*>& val_map) {
    const char* what = inst.op == BronzeOp::Leave ? "leave" : "resume.call";
    if (current_ast_ == nullptr || inst.index >= current_ast_->functions.size() ||
        inst.index >= resolved_names_.size() || resolved_names_[inst.index].empty()) {
        inst_error(inst, std::string(what) + " names no function of the module");
        return nullptr;
    }
    const BronzeFunction& g_ast = current_ast_->functions[inst.index];
    Function* g = fn->parent()->get_function(resolved_names_[inst.index]);
    if (g == nullptr) {
        inst_error(inst, std::string(what) + " names a function with no body");
        return nullptr;
    }
    const bool g_has_argv = argv_block_words(this, g_ast) > 0;
    const size_t words = inst.operands.size() + (options_.per_thread_module_data ? 1 : 0) + (g_has_argv ? 1 : 0);
    // lower_function sized the buffer for every leave and resume.call.
    if (leave_state_ == nullptr || words > leave_state_words_) {
        inst_error(inst, std::string(what) + "'s state does not fit its buffer");
        return nullptr;
    }
    if (g_has_argv && argv_block_ == nullptr) {
        inst_error(inst, std::string(what) + " without an argv block for its twin");
        return nullptr;
    }

    const auto& types = il_types();
    int32_t at = 0;
    auto store_word = [&](Value* word) {
        b.build_store(Type::i64(), leave_state_, at, word);
        at += 8;
    };
    for (uint32_t opd : inst.operands) {
        Value* v = get_val_by_id(opd, b, val_map);
        const auto ty = types.find(opd);
        if (v == nullptr || ty == types.end()) {
            inst_error(inst, std::string(what) + " state value %" + std::to_string(opd) + " is not defined here");
            return nullptr;
        }
        switch (ty->second) {
            case BronzeType::F64:
                store_word(box_f64_bits(b, ensure_type(v, Type::f64(), b)));
                break;
            case BronzeType::I32:
                store_word(b.build_zext_i64(ensure_type(v, Type::i32(), b)));
                break;
            case BronzeType::Bool:
                store_word(b.build_zext_i64(b.build_and(ensure_type(v, Type::i32(), b), b.build_iconst_i32(1))));
                break;
            default:
                store_word(ensure_type(v, Type::i64(), b));
                break;
        }
    }
    if (options_.per_thread_module_data) store_word(module_delta(b));
    if (g_has_argv) b.build_store(argv_block_->type(), leave_state_, at, argv_block_);
    return g;
}

bool IlLowering::lower_leave(const BronzeInstruction& inst, Builder& b, Function* fn,
                             std::unordered_map<uint32_t, Value*>& val_map) {
    // An exit: the function's speculation failed, and the rest of the call is
    // the twin's. Laid out after the main line.
    b.current_block()->set_cold();
    Function* g = stage_twin_state(inst, b, fn, val_map);
    if (g == nullptr) return false;
    Value* r = b.build_call(brass::resume_twin_name(g->name()), g->return_type(),
                            {b.build_iconst_i32(static_cast<int32_t>(inst.imm_i64)), leave_state_});
    if (fn->return_type().is_void()) {
        b.build_ret_void();
    } else {
        b.build_ret(ensure_type(r, fn->return_type(), b));
    }
    return true;
}

// `resume.call @G, id, state...` (il_op.h): the same twin call a leave makes,
// with the same state words, but its result is an ordinary value of this
// function and control goes on. It sits in an exit block of an inlined body
// (opt/inline), which the leave after it marks cold. A throw out of the twin
// propagates as from any call: the block has no handler (resume_rules.h keeps
// inlined exits out of protected blocks), so it leaves this function too.
bool IlLowering::lower_resume_call(const BronzeInstruction& inst, Builder& b, Function* fn,
                                   std::unordered_map<uint32_t, Value*>& val_map) {
    Function* g = stage_twin_state(inst, b, fn, val_map);
    if (g == nullptr) return false;
    Value* r = b.build_call(brass::resume_twin_name(g->name()), g->return_type(),
                            {b.build_iconst_i32(static_cast<int32_t>(inst.imm_i64)), leave_state_});
    if (inst.result_id != UINT32_MAX) {
        if (g->return_type().is_void()) return inst_error(inst, "resume.call has a result where G returns void");
        set_inst_result(inst.result_id, r, b, val_map);
    }
    return true;
}

bool IlLowering::build_resume_twins(Module& mod) {
    std::vector<Function*> generic;
    for (Function* fn : mod.functions()) {
        if (fn && !fn->resume_points().empty() && !fn->has_coro_frame_layout()) generic.push_back(fn);
    }
    for (Function* fn : generic) {
        std::string why;
        if (brass::build_resume_twin(*fn, mod, brass::resume_twin_name(fn->name()), {}, &why) == nullptr) {
            has_error_ = true;
            if (diag_) {
                diag_->error(SourceLocation("", 0, 0),
                             "il2mir: no resume twin for " + std::string(fn->name()) + ": " + why);
            }
            return false;
        }
        // The twin is cut: G's own points are guards that never fail, and go.
        for (BasicBlock* bb : fn->blocks()) {
            if (!bb) continue;
            std::vector<Instruction*> guards;
            for (Instruction* inst : *bb) {
                if (inst && inst->opcode() == Opcode::guard && fn->get_resume_target(inst->resume_id()) != nullptr) {
                    guards.push_back(inst);
                }
            }
            for (Instruction* g : guards) bb->remove_instruction(g);
        }
        std::vector<uint32_t> ids;
        for (const auto& [id, target] : fn->resume_points()) ids.push_back(id);
        for (uint32_t id : ids) fn->remove_resume_point(id);
    }
    return true;
}

}  // namespace il2mir

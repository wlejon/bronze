#include "il_lowering_calls.h"
#include "il_lowering.h"
#include "il_abi.h"
#include "il_lowering_direct.h"
#include "il_lowering_ops.h"
#include "il_accessor_direct.h"
#include "il_array_method.h"
#include "il_is_array.h"
#include "il_native_fast.h"
#include "il/il.h"
#include <brass/mir/module.hpp>
#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace il2mir {

bool is_call_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::Construct:
        case BronzeOp::ConstructSpread:
        case BronzeOp::MethodCall:
        case BronzeOp::MethodCallSpread:
        case BronzeOp::SuperCall:
        case BronzeOp::SuperCallSpread:
        case BronzeOp::SuperGet:
        case BronzeOp::SuperSet:
        case BronzeOp::CallDynamic:
        case BronzeOp::DynamicCallSpread:
        case BronzeOp::FuncRef:
        case BronzeOp::CreateFunc:
        case BronzeOp::MethodIcIs:
        case BronzeOp::AccessorIcIs:
        case BronzeOp::MethodSlotFn:
        case BronzeOp::ClosureIs:
        case BronzeOp::ClosureEnv:
            return true;
        default:
            return false;
    }
}

namespace {

// Whether the method-call `site`'s cache answers `callee` for `recv`: a
// plain receiver whose shape a DIRECT entry (way 0 or way 1) latched, at the
// current epoch, with that entry's code pointer the callee's wrapper. The
// direct edge's guard, and `method.ic.is`, the guard of an inlined body.
//
// WAY 0 FIRST. A monomorphic site — every site this guard is hot at, since a
// site that keeps missing way 0 is one whose inlined body rarely runs — is
// answered by way 0 alone, so way 1's three loads and compares are a block of
// their own, named cold for brass's block layout (codegen/block_layout.cpp)
// and reached only when way 0 says no. Testing both ways branch-free cost
// every hit the second way's words; the answer is the same either way, since
// way 1 is consulted exactly when way 0 does not answer. The result is the
// merge's parameter, a constant 1 on the way-0 edge, which brass's jump
// threading folds into the branch that consumes it.
//
// `keyed` is the receiver's keyed shape (PropertyLoweringHelper::keyed_shape),
// often the shape memo's (il_shape_memo.cpp): equal to a way's shape word
// exactly when the receiver is plain and of that shape.
Value* direct_method_ic_hit(Builder& b, Value* keyed, Value* site, Value* row, const std::string& callee) {
    constexpr int32_t kWord = 8;
    constexpr int64_t kDirectFormLimit = int64_t{1} << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
    Value* target = b.build_func_addr("__wrapper_" + callee);
    // A DIRECT entry answers only while the prototype-mutation epoch it was
    // latched at is current: `C.prototype.m = f` moves no shape, only the
    // epoch (bronze_abi.h, the METHOD-CALL site contract).
    Module* mod = b.current_block()->parent()->parent();
    Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                     : b.build_call("bronze_tls_block_addr", Type::i64(), {});
    // Read in the way that compares it, in front of the compare, which then
    // takes the entry's word as a memory operand.
    auto epoch_is = [&](Value* base, int32_t off) {
        Value* epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF);
        return b.build_eq(epoch, b.build_load(Type::i64(), base, off));
    };

    BasicBlock* cur = b.current_block();
    const std::string prefix = "mic_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* way0_entry = b.append_block(prefix + "_way0");
    BasicBlock* way1_bb = b.append_block(prefix + "_way1_cold");
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* hit = b.add_block_param(merge, Type::i32());

    // Way 0's words from the site's method row (bronze_abi_layout.h, "the
    // METHOD ROWS"), a line shared with the sites around it.
    const MethodWay0 w = method_way0(site, row);
    b.position_at_end(cur);
    b.build_br_if(b.build_eq(keyed, b.build_load(Type::i64(), w.base, w.shape)), way0_entry, way1_bb);

    b.position_at_end(way0_entry);
    Value* way0 = b.build_and(
        b.build_ult(b.build_load(Type::i64(), w.base, w.arity), b.build_iconst_i64(kDirectFormLimit)),
        b.build_and(b.build_eq(b.build_load(Type::ptr(), w.base, w.code), target),
                    epoch_is(w.base, w.epoch)));
    b.build_br_if(way0, merge, {b.build_iconst_i32(1)}, way1_bb, {});

    // Way 0 did not answer, so way 1 is the whole answer; at most one way
    // names a shape (an install rewrites an entry in place).
    b.position_at_end(way1_bb);
    Value* way1 = b.build_and(
        b.build_and(
            b.build_eq(keyed, b.build_load(Type::i64(), site, BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD * kWord)),
            b.build_eq(b.build_load(Type::ptr(), site, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD * kWord), target)),
        epoch_is(site, BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD * kWord));
    b.build_br(merge, {way1});
    way1_bb->set_cold();

    b.position_at_end(merge);
    return hit;
}

// `recv.key(...)` whose callee lowering named (lower/direct_method_table.h).
// The name is the nearest declaration at or above the class inference gave
// the receiver, which is a GUESS: a subclass override, a monkey-patch or a
// receiver of another class entirely resolves `key` to another function. So
// the direct call runs only when the site's method IC answers the guessed
// function for this receiver — a plain object whose shape a DIRECT entry
// (way 0 or way 1) latched, with that entry's code pointer the callee's
// wrapper. Everything else, the first call that latches the entry included,
// takes the generic call, which dispatches on the receiver in hand.
Value* lower_direct_method_call(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* recv,
                                const std::vector<Value*>& args, Value* site, const std::string& callee,
                                const std::string& entry_name, Function* direct_fn, bool this_param,
                                const std::function<Value*(Builder&)>& recv_after) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    // The miss of an inlined body's guard (IlLowering::find_guard_misses):
    // the generic call alone.
    const bool missed = lowering->guard_missed(inst);

    BasicBlock* cur = b.current_block();
    const std::string prefix = "direct_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = missed ? nullptr : b.append_block(prefix + "_fast");
    BasicBlock* slow = missed ? cur : b.append_block(prefix + "_slow");
    BasicBlock* merge = missed ? nullptr : b.append_block(prefix + "_merge");
    Value* result = missed ? nullptr : b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    // The site's method row, which the miss's stub call takes too: computed
    // here, ahead of the receiver test that may branch to it.
    Value* row = pl.method_row(b, inst.ic_index);
    if (!missed) {
        // A receiver the memo does not hold is tested on a path only a plain
        // receiver reaches, the rest going to the generic call; the call ends
        // the memo, so its shape word is not remembered.
        Value* keyed = lowering->memo_lookup(inst.operands[0]);
        if (!keyed) keyed = pl.plain_shape_or_miss(b, recv, slow);
        Value* hit = direct_method_ic_hit(b, keyed, site, row, callee);
        lowering->spec().emit_branch(b, hit, fast, slow, SpecKind::Property,
                                     "()" + std::string(pl.key_tag(inst.index)));
    }
    // The generic dispatch takes every argument boxed. A typed edge's
    // unboxed operands are boxed here, on the path that needs them.
    std::vector<Value*> boxed_args;
    boxed_args.reserve(args.size());
    const auto& params = direct_fn->param_types();
    // A callee that never reads `this` takes no receiver parameter.
    const size_t first = this_param ? 1 : 0;
    for (size_t a = 0; a < args.size(); ++a) {
        boxed_args.push_back(box_call_arg(lowering, b, args[a], a + first < params.size() ? params[a + first] : Type::i64()));
    }
    Value* argv = lowering->stage_argv(b, boxed_args);
    Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(args.size()));
    Value* key_id = lowering->get_key_id(b, inst.index);
    // The guard's miss is the method stub where the module has one: the
    // receivers a polymorphic site's two DIRECT ways do not hold are still
    // answered from the thread's table or a SLOT entry without the runtime.
    Value* generic = direct_method_ic_here(lowering)
                         ? b.build_call("__bronze_method_call_ic", Type::i64(),
                                        {recv, key_id, argc_val, argv, site, row ? row : b.build_iconst_i64(0)})
                         : b.build_call("bronze_call_method", Type::i64(), {recv, key_id, argc_val, argv, site});
    if (missed) {
        // The guard's cell pair learns this receiver's shape, when the call
        // just made was of the guessed callee.
        if (const BronzeInstruction* guard = lowering->method_cell_guard(inst);
            guard && pl.static_cells_cover(guard->static_cell, 2)) {
            emit_method_cell_fill(lowering, b, recv_after(b), guard->static_cell, inst.index, guard->callee_name);
        }
        return generic;
    }
    b.build_br(merge, {generic});

    b.position_at_end(fast);
    std::vector<Value*> call_args;
    if (this_param) call_args.push_back(lowering->ensure_type(recv, params[0], b));
    for (size_t p = first; p < params.size(); ++p) {
        const Type pt = params[p];
        Value* a = p - first < args.size() ? args[p - first] : nullptr;
        // An omitted argument's parameter is tagged (the call site's fit test).
        call_args.push_back(lowering->ensure_type(
            a ? a : b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag)), pt, b));
    }
    Value* direct = b.build_call(entry_name, direct_fn->return_type(), Span<Value* const>(call_args.data(), call_args.size()));
    Value* boxed = direct_fn->return_type() == Type::void_type()
        ? b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag))
        : lowering->ensure_type(direct, Type::i64(), b);
    b.build_br(merge, {boxed});

    b.position_at_end(merge);
    return result;
}

} // namespace

// One alloca.tagged of the widest such call's arguments, so they stay rooted,
// and are updated, while the callee or the helper can collect. Nested calls
// cannot overlap: an argument is a value already computed by the time its call
// stores it, and the stores happen right before the call.
uint32_t argv_block_words(const IlLowering* lowering, const BronzeFunction& fn_ast) {
    if (lowering) const_cast<IlLowering*>(lowering)->begin_builtin_lowering(fn_ast);
    const bool direct = lowering && lowering->options().direct_js_calls;
    uint32_t widest = 0;
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::MethodCall && inst.param_count > widest &&
                !method_call_is_speculated_builtin(lowering, inst)) {
                widest = inst.param_count;
            }
            // A transition run stages its keys for the publish (il_transition_run.cpp).
            if ((inst.op == BronzeOp::PropSet || inst.op == BronzeOp::PropDefine) &&
                inst.run_cell != BronzeInstruction::kNoTransitionRun &&
                inst.run_length > widest) {
                widest = inst.run_length;
            }
            if ((inst.op == BronzeOp::Construct || inst.op == BronzeOp::SuperCall || inst.op == BronzeOp::CallDynamic) &&
                (inst.param_count > 16 || (direct && inst.op != BronzeOp::SuperCall)) && inst.param_count > widest) {
                widest = inst.param_count;
            }
        }
    }
    return widest;
}

bool lower_call_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* fn,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
) {
    auto get_opd = [&](size_t idx) -> Value* {
        if (idx < inst_ast.operands.size()) {
            uint32_t id = inst_ast.operands[idx];
            if (lowering) {
                return lowering->get_val_by_id(id, b, val_map);
            }
            if (val_map.count(id)) return val_map[id];
        }
        return nullptr;
    };

    auto ensure_type = [&](Value* val, Type target_type) -> Value* {
        return lowering ? lowering->ensure_type(val, target_type, b) : val;
    };

    switch (inst_ast.op) {
        case BronzeOp::Construct: {
            Value* ctor = ensure_type(get_opd(0), Type::i64());
            uint32_t argc = inst_ast.param_count;
            if (direct_entry_here(lowering)) {
                std::vector<Value*> args;
                for (uint32_t i = 0; i < argc; ++i) args.push_back(ensure_type(get_opd(1 + i), Type::i64()));
                res_val = lower_construct_direct(lowering, b, ctor, args);
            } else if (argc <= 16) {
                std::vector<Value*> call_args = {ctor};
                for (uint32_t i = 0; i < argc; ++i) {
                    call_args.push_back(ensure_type(get_opd(1 + i), Type::i64()));
                }
                std::string helper = "bronze_construct_" + std::to_string(argc);
                res_val = b.build_call(helper, Type::i64(), call_args);
            } else {
                std::vector<Value*> args;
                for (uint32_t i = 0; i < argc; ++i) args.push_back(get_opd(1 + i));
                Value* argv = lowering->stage_argv(b, args);
                Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
                res_val = b.build_call("bronze_construct_n", Type::i64(), {ctor, argc_val, argv});
            }
            break;
        }

        case BronzeOp::ConstructSpread: {
            Value* callee = ensure_type(get_opd(0), Type::i64());
            Value* args = ensure_type(get_opd(1), Type::i64());
            res_val = b.build_call("bronze_construct_spread", Type::i64(), {callee, args});
            break;
        }

        // The guard of an inlined method body, which is the direct edge's: no
        // site (a module built without IC sites) answers no, and the body's
        // guard sends every call to the original method.call.
        case BronzeOp::MethodIcIs: {
            Value* recv = ensure_type(get_opd(0), Type::i64());
            Value* site = lowering ? lowering->prop_lowering().ic_site(b, inst_ast.ic_index) : nullptr;
            // A guard reads and decides; it moves no shape, so the memo
            // stays (il_shape_memo.cpp).
            if (!site) {
                res_val = b.build_iconst_i32(0);
                break;
            }
            PropertyLoweringHelper& pl = lowering->prop_lowering();
            Value* keyed = lowering->memo_keyed(b, inst_ast.operands[0], recv);
            const auto row_hit = [&] {
                return direct_method_ic_hit(b, keyed, site, pl.method_row(b, inst_ast.ic_index), inst_ast.callee_name);
            };
            // A guard whose miss fills its cell pair asks the pair first: two
            // compares where the site's cache entry takes four. The entry
            // still answers what the pair does not hold (a site's second
            // shape; the pair holds one).
            if (lowering->method_cell_filled(inst_ast) && pl.static_cells_cover(inst_ast.static_cell, 2)) {
                BasicBlock* cur = b.current_block();
                const std::string prefix = "mcell_" + std::to_string(cur->parent()->next_block_id());
                BasicBlock* row_bb = b.append_block(prefix + "_row_cold");
                BasicBlock* merge = b.append_block(prefix + "_merge");
                Value* hit = b.add_block_param(merge, Type::i32());
                b.position_at_end(cur);
                b.build_br_if(accessor_cell_hit(lowering, b, recv, inst_ast.static_cell, keyed), merge,
                              {b.build_iconst_i32(1)}, row_bb, {});
                b.position_at_end(row_bb);
                Value* row = row_hit();
                // The entry answered what the pair could not: the pair learns
                // this receiver's shape (il_accessor_direct.h).
                BasicBlock* asked = b.current_block();
                BasicBlock* learn = b.append_block(prefix + "_learn_cold");
                b.position_at_end(asked);
                b.build_br_if(row, learn, {}, merge, {b.build_iconst_i32(0)});
                b.position_at_end(learn);
                emit_method_cell_learn(lowering, b, recv, keyed, inst_ast.static_cell,
                                       lowering->method_cell_key(inst_ast), inst_ast.callee_name);
                b.build_br(merge, {b.build_iconst_i32(1)});
                learn->set_cold();
                row_bb->set_cold();
                b.position_at_end(merge);
                res_val = hit;
            } else {
                res_val = row_hit();
            }
            lowering->memo_keep();
            break;
        }

        // An inlined own-slot callback's tests (il_lowering_direct.h). Loads
        // only, which move no shape, so the memo stays. Without the direct
        // forms (no image build) no closure is entered directly, so none is
        // inlined either: the slot reads `undefined` and the test says no.
        case BronzeOp::MethodSlotFn: {
            Value* recv = ensure_type(get_opd(0), Type::i64());
            Value* site = lowering && direct_method_ic_here(lowering)
                              ? lowering->prop_lowering().ic_site(b, inst_ast.ic_index)
                              : nullptr;
            res_val = site ? lower_method_slot_fn(lowering, b, recv,
                                                  lowering->memo_keyed(b, inst_ast.operands[0], recv), site,
                                                  lowering->prop_lowering().method_row(b, inst_ast.ic_index))
                           : b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag));
            if (lowering) lowering->memo_keep();
            break;
        }
        case BronzeOp::ClosureIs: {
            Value* f = ensure_type(get_opd(0), Type::i64());
            res_val = direct_method_ic_here(lowering) ? lower_closure_is(lowering, b, f, inst_ast.callee_name)
                                                      : b.build_iconst_i32(0);
            if (lowering) lowering->memo_keep();
            break;
        }
        case BronzeOp::ClosureEnv: {
            res_val = lower_closure_env(b, ensure_type(get_opd(0), Type::i64()));
            if (lowering) lowering->memo_keep();
            break;
        }

        // The guard of an inlined accessor body: the direct accessor site's
        // own (il_accessor_direct.h). Where sites take no direct form the
        // answer is no, and the body's guard sends every access to the
        // original prop.get or prop.set.
        //
        // Like `method.ic.is` it reads and decides and moves no shape, so it
        // asks the shape memo for the receiver's keyed shape and leaves the
        // memo standing: the inlined body's own static sites on `this` (the
        // getter's `this._x`, the setter's store and `_onChangeCallback`) then
        // pay only their cell compare.
        case BronzeOp::AccessorIcIs: {
            Value* recv = ensure_type(get_opd(0), Type::i64());
            if (!accessor_direct_here(lowering, inst_ast)) {
                res_val = b.build_iconst_i32(0);
            } else {
                res_val = accessor_cell_hit(lowering, b, recv, inst_ast.static_cell,
                                            lowering->memo_keyed(b, inst_ast.operands[0], recv));
                lowering->memo_keep();
            }
            break;
        }

        case BronzeOp::MethodCall: {
            Value* recv = ensure_type(get_opd(0), Type::i64());
            uint32_t argc = inst_ast.param_count;
            std::string callee = lowering ? lowering->resolve_callee(inst_ast.callee_name) : inst_ast.callee_name;
            if (!callee.empty() && std::isdigit(static_cast<unsigned char>(callee[0]))) {
                uint32_t f_idx = static_cast<uint32_t>(std::stoul(callee));
                if (lowering && lowering->current_ast() && f_idx < lowering->current_ast()->functions.size()) {
                    callee = lowering->resolve_callee(lowering->current_ast()->functions[f_idx].name);
                }
            }
            Function* direct_fn = (!callee.empty() && fn && fn->parent()) ? fn->parent()->get_function(callee) : nullptr;
            // A typed edge (lower/typed_entry.h): the guard keeps `callee`'s
            // identity and the fast path calls its typed entry, whose typed
            // parameters this site's operands already are — the IL verifier
            // checked that, and a mismatch here is a hard error, never a
            // conversion: converting a boxed value to f64 is a bitcast.
            std::string entry_name = callee;
            // Per argument, how it is held: boxed (`i64`) unless a typed edge
            // passes it unboxed, which is what boxing it for the generic
            // dispatch has to know.
            std::vector<Type> arg_kinds(argc, Type::i64());
            if (direct_fn && lowering && !inst_ast.typed_callee_name.empty()) {
                const std::string typed = lowering->resolve_callee(inst_ast.typed_callee_name);
                Function* entry_fn = fn->parent()->get_function(typed);
                const auto& eparams = entry_fn ? entry_fn->param_types() : direct_fn->param_types();
                bool fits = entry_fn && eparams.size() >= argc + 1 &&
                            eparams.size() == direct_fn->param_types().size();
                // An IL bool is held in an i32 and passed as an i8: the same
                // 0/1, narrowed by the fast path's ensure_type. A 64-bit or
                // tagged operand is a boxed value, never a bool.
                auto same_repr = [](Type have, Type want) {
                    if (have == want) return true;
                    return want == Type::i8() && (have == Type::i32() || have == Type::i16());
                };
                for (size_t p = 1; fits && p < eparams.size(); ++p) {
                    if (eparams[p] == direct_fn->param_types()[p]) continue;
                    Value* a = p <= argc ? get_opd(p) : nullptr;
                    fits = a && same_repr(a->type(), eparams[p]);
                }
                if (!fits) {
                    lowering->set_has_error(true);
                    if (lowering->diag()) {
                        lowering->diag()->error(SourceLocation("", 0, 0),
                                                "il2mir: typed method entry '" + inst_ast.typed_callee_name +
                                                    "' does not fit its call site's operands");
                    }
                    return false;
                }
                entry_name = typed;
                direct_fn = entry_fn;
                for (size_t p = 1; p < eparams.size() && p <= argc; ++p) arg_kinds[p - 1] = eparams[p];
            }
            // The callee takes `argc` arguments, or more whose parameters are
            // tagged (`update = true`), which an omitted argument fills with
            // undefined as an adapting call would. Fewer is fine too: the
            // fast path passes the first ones and drops the rest, which a
            // callee with no `arguments` object and no rest parameter (the
            // terms below) cannot observe.
            bool this_param = true;
            auto params_fit = [&]() {
                const auto& params = direct_fn->param_types();
                for (size_t p = argc + (this_param ? 1 : 0); p < params.size(); ++p) {
                    if (!params[p].is_tagged() && params[p] != Type::i64()) return false;
                }
                return true;
            };
            bool can_direct = false;
            if (direct_fn && lowering) {
                auto it_m = lowering->options().function_meta.find(callee);
                if (it_m != lowering->options().function_meta.end()) {
                    // A callee with no `this` parameter is entered without
                    // the receiver, which it never reads.
                    this_param = it_m->second.needs_this;
                    can_direct = !it_m->second.needs_arguments && !it_m->second.has_rest_param &&
                                 !it_m->second.needs_env && params_fit();
                } else {
                    can_direct = (direct_fn->param_types().size() == argc + 1);
                }
            } else if (direct_fn) {
                can_direct = (direct_fn->param_types().size() == argc + 1);
            }

            const bool poly_recv = (inst_ast.imm_i64 & bronze::il::Instruction::kMethodPolyShapes) != 0;
            // Code that runs once dispatches generically: the direct form's
            // guard is only size there.
            if (can_direct && lowering && lowering->run_once()) can_direct = false;
            // A speculated builtin (a `Math` member) is a native, so a direct
            // JS target there is a wrong guess; the builtin form stages no argv,
            // which argv_block_words counts on.
            if (can_direct && method_call_is_speculated_builtin(lowering, inst_ast)) can_direct = false;
            if (const ArrayInline arr = can_direct ? ArrayInline::None : array_method_inline(lowering, inst_ast);
                arr != ArrayInline::None) {
                if (Value* site = lowering->prop_lowering().ic_site(b, inst_ast.ic_index)) {
                    std::vector<Value*> args;
                    std::vector<ArrayPushArg> push_args;
                    args.reserve(argc);
                    push_args.reserve(argc);
                    for (size_t a = 0; a < argc; ++a) {
                        Value* raw_opd = get_opd(1 + a);
                        const bool is_num = arg_kinds[a] == Type::f64() || raw_opd->type() == Type::f64() ||
                                            boxed_f64_source(raw_opd) != nullptr ||
                                            lowering->is_number_value(inst_ast.operands[1 + a]);
                        Value* boxed = box_call_arg(lowering, b, raw_opd, arg_kinds[a]);
                        args.push_back(boxed);
                        push_args.push_back({boxed, is_num});
                    }
                    Value* row = lowering->prop_lowering().method_row(b, inst_ast.ic_index);
                    const auto rows = native_fast_rows(lowering, inst_ast);
                    const auto ic = [&](Builder& bb) {
                        return lower_method_call_ic(lowering, bb, recv, lowering->get_key_id(bb, inst_ast.index),
                                                    args, site, row, poly_recv);
                    };
                    // A push the inline arm leaves (a full block) enters the
                    // native at the site.
                    res_val = lower_array_method_inline(
                        lowering, b, arr, recv, lowering->is_array_value(inst_ast.operands[0]), push_args, [&](Builder& bb) {
                        if (rows.empty()) return ic(bb);
                        return lower_native_fast(lowering, bb, rows, recv, args, site, [&](Builder& sb) {
                            return lower_method_stub_call(lowering, sb, recv, lowering->get_key_id(sb, inst_ast.index),
                                                          args, site, row);
                        });
                    });
                    break;
                }
            }
            if (const auto rows = can_direct ? std::vector<const NativeFastRow*>{} : native_fast_rows(lowering, inst_ast);
                !rows.empty()) {
                if (Value* site = lowering->prop_lowering().ic_site(b, inst_ast.ic_index)) {
                    std::vector<Value*> args;
                    for (size_t a = 0; a < argc; ++a) args.push_back(box_call_arg(lowering, b, get_opd(1 + a), arg_kinds[a]));
                    Value* row = lowering->prop_lowering().method_row(b, inst_ast.ic_index);
                    // The stub alone behind the arms: way 0 is a plain
                    // object's DIRECT entry only where a program gives one
                    // a member of a builtin's name.
                    res_val = lower_native_fast(lowering, b, rows, recv, args, site, [&](Builder& bb) {
                        return lower_method_stub_call(lowering, bb, recv, lowering->get_key_id(bb, inst_ast.index),
                                                      args, site, row);
                    });
                    break;
                }
            }
            if (!can_direct && lowering && method_call_is_speculated_builtin(lowering, inst_ast)) {
                if (Value* site = lowering->prop_lowering().ic_site(b, inst_ast.ic_index)) {
                    res_val = lowering->try_lower_builtin_method_call(inst_ast, b, recv, arg_kinds, val_map, site);
                    if (res_val) break;
                }
            }
            if (!can_direct && argc >= 1 && lowering) {
                if (Value* site = lowering->prop_lowering().ic_site(b, inst_ast.ic_index)) {
                    std::vector<Value*> args;
                    for (size_t a = 0; a < argc; ++a) args.push_back(box_call_arg(lowering, b, get_opd(1 + a), arg_kinds[a]));
                    res_val = lower_is_array_call(lowering, inst_ast, b, recv, args, site, poly_recv);
                    if (res_val) break;
                }
            }
            if (Value* site = (lowering ? lowering->prop_lowering().ic_site(b, inst_ast.ic_index) : nullptr)) {
                Value* row = lowering->prop_lowering().method_row(b, inst_ast.ic_index);
                std::vector<Value*> args;
                for (size_t a = 0; a < argc; ++a) args.push_back(get_opd(1 + a));
                if (can_direct) {
                    const auto direct = [&](Builder& bb) {
                        return lower_direct_method_call(
                            lowering, inst_ast, bb, recv, args, site, callee, entry_name, direct_fn, this_param,
                            [&](Builder&) { return ensure_type(get_opd(0), Type::i64()); });
                    };
                    // A receiver may hold its callee in an own slot (three.js's
                    // `_onChangeCallback`, a prototype no-op shadowed by a
                    // closure on each instance): the method cache's SLOT form
                    // never answers the direct guard, so the slot is tried
                    // first and the guessed prototype method after it. An
                    // Array receiver is never the plain object a SLOT entry
                    // names, so its site keeps the direct form alone.
                    const int64_t hints = inst_ast.imm_i64;
                    if ((hints & bronze::il::Instruction::kMethodOwnCallee) != 0 &&
                        (hints & bronze::il::Instruction::kMethodRecvArray) == 0 &&
                        direct_method_ic_here(lowering) && !lowering->run_once()) {
                        std::vector<Value*> boxed;
                        boxed.reserve(argc);
                        for (size_t a = 0; a < argc; ++a) boxed.push_back(box_call_arg(lowering, b, args[a], arg_kinds[a]));
                        res_val = lower_method_slot_inline(lowering, b, recv, boxed, site, row, direct);
                        break;
                    }
                    res_val = direct(b);
                    break;
                }
                for (size_t a = 0; a < argc; ++a) args[a] = box_call_arg(lowering, b, args[a], arg_kinds[a]);
                Value* key_id = lowering->get_key_id(b, inst_ast.index);
                if (direct_method_ic_here(lowering)) {
                    // `Class.method(...)`: the statics box's SLOT form at the
                    // site, as the stub would answer it after its other arms.
                    if (inst_ast.is_fn_recv && direct_entry_here(lowering)) {
                        res_val = lower_method_fn_recv(lowering, b, recv, args, site, row, [&](Builder& bb) {
                            return lower_method_call_ic(lowering, bb, recv, key_id, args, site, row, poly_recv);
                        });
                        break;
                    }
                    if ((inst_ast.imm_i64 & bronze::il::Instruction::kMethodOwnCallee) != 0 && !lowering->run_once()) {
                        res_val = lower_method_slot_inline(lowering, b, recv, args, site, row, [&](Builder& bb) {
                            return lower_method_call_ic(lowering, bb, recv, key_id, args, site, row, poly_recv);
                        });
                        break;
                    }
                    res_val = lower_method_call_ic(lowering, b, recv, key_id, args, site, row, poly_recv);
                    break;
                }
                Value* argv = lowering->stage_argv(b, args);
                Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
                res_val = b.build_call("bronze_call_method", Type::i64(),
                                       {recv, key_id, argc_val, argv, site});
            } else {
                Value* null_entry = b.build_iconst_i64(0);
                Value* key_id = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
                Value* method = b.build_call("bronze_prop_get", Type::i64(), {recv, key_id, null_entry});
                recv = ensure_type(get_opd(0), Type::i64());
                std::vector<Value*> dyn_args = {method, recv};
                for (size_t a = 0; a < argc; ++a) {
                    Value* arg = get_opd(1 + a);
                    dyn_args.push_back(lowering ? box_call_arg(lowering, b, arg, arg_kinds[a]) : ensure_type(arg, Type::i64()));
                }
                res_val = build_call_dynamic(b, dyn_args, lowering);
            }
            break;
        }

        case BronzeOp::MethodCallSpread: {
            Value* this_val = ensure_type(get_opd(0), Type::i64());
            Value* args = ensure_type(get_opd(1), Type::i64());
            Value* key_id = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
            Value* site = lowering ? lowering->prop_lowering().ic_site(b, inst_ast.ic_index) : nullptr;
            Value* entry = site ? site : b.build_iconst_i64(0);
            res_val = b.build_call("bronze_call_method_spread", Type::i64(), {this_val, key_id, args, entry});
            break;
        }

        case BronzeOp::SuperCall: {
            Value* base_ctor = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            uint32_t argc = inst_ast.param_count;
            if (argc <= 16) {
                std::vector<Value*> super_args = {base_ctor, this_val};
                for (size_t a = 0; a < argc; ++a) {
                    super_args.push_back(ensure_type(get_opd(2 + a), Type::i64()));
                }
                std::string helper = "bronze_super_call_" + std::to_string(argc);
                res_val = b.build_call(helper, Type::i64(), Span<Value* const>(super_args.data(), super_args.size()));
            } else {
                std::vector<Value*> args;
                for (size_t a = 0; a < argc; ++a) args.push_back(get_opd(2 + a));
                Value* argv = lowering->stage_argv(b, args);
                Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
                res_val = b.build_call("bronze_super_call_n", Type::i64(), {base_ctor, this_val, argc_val, argv});
            }
            break;
        }

        case BronzeOp::SuperCallSpread: {
            Value* base_ctor = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* args = ensure_type(get_opd(2), Type::i64());
            res_val = b.build_call("bronze_super_call_spread", Type::i64(), {base_ctor, this_val, args});
            break;
        }

        case BronzeOp::SuperGet: {
            Value* proto = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* kidx = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
            res_val = b.build_call("bronze_super_get", Type::i64(), {proto, kidx, this_val});
            break;
        }

        case BronzeOp::SuperSet: {
            Value* proto = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* val = ensure_type(get_opd(2), Type::i64());
            Value* kidx = lowering ? lowering->get_key_id(b, inst_ast.index) : b.build_iconst_i32(inst_ast.index);
            Value* strict = b.build_iconst_i32(inst_ast.imm_i64 != 0 ? 1 : 0);
            b.build_call("bronze_super_set", Type::void_type(), {proto, kidx, this_val, val, strict});
            break;
        }

        case BronzeOp::CallDynamic: {
            if (lowering) {
                res_val = lowering->try_lower_builtin_dynamic_call(inst_ast, b, val_map);
                if (res_val) break;
            }
            Value* callee_val = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            uint32_t argc = inst_ast.param_count;
            std::vector<Value*> dyn_args = {callee_val, this_val};
            for (size_t a = 0; a < argc; ++a) {
                dyn_args.push_back(ensure_type(get_opd(2 + a), Type::i64()));
            }
            if (direct_entry_here(lowering)) {
                res_val = lower_closure_call(lowering, b, callee_val, this_val,
                                             std::vector<Value*>(dyn_args.begin() + 2, dyn_args.end()));
                break;
            }
            res_val = build_call_dynamic(b, dyn_args, lowering);
            break;
        }

        case BronzeOp::DynamicCallSpread: {
            Value* callee = ensure_type(get_opd(0), Type::i64());
            Value* this_val = ensure_type(get_opd(1), Type::i64());
            Value* args = ensure_type(get_opd(2), Type::i64());
            res_val = b.build_call("bronze_dynamic_call_spread", Type::i64(), {callee, this_val, args});
            break;
        }

        case BronzeOp::FuncRef: {
            std::string callee = inst_ast.callee_name;
            if (!callee.empty() && std::isdigit(static_cast<unsigned char>(callee[0]))) {
                uint32_t f_idx = static_cast<uint32_t>(std::stoul(callee));
                if (lowering && lowering->current_ast() && f_idx < lowering->current_ast()->functions.size()) {
                    callee = lowering->current_ast()->functions[f_idx].name;
                }
            }
            callee = lowering ? lowering->resolve_callee(callee) : callee;
            Value* code_addr = b.build_func_addr("__wrapper_" + callee);
            uint32_t arity = 0;
            uint32_t length = 0;
            uint32_t name_key_val = 0xFFFFFFFFu;
            uint32_t fn_flags_val = 0x03;
            if (lowering) {
                auto it_meta = lowering->options().function_meta.find(callee);
                if (it_meta != lowering->options().function_meta.end()) {
                    fn_flags_val = it_meta->second.fn_flags;
                    name_key_val = it_meta->second.name_key;
                    length = it_meta->second.required_args;
                    arity = it_meta->second.adapt_arity;
                } else if (lowering->current_ast()) {
                    for (const auto& f : lowering->current_ast()->functions) {
                        if (f.name == callee) {
                            length = static_cast<uint32_t>(f.params.size());
                            arity = length;
                            break;
                        }
                    }
                }
            }
            Value* arity_val = b.build_iconst_i32(static_cast<int32_t>(arity));
            Value* length_val = b.build_iconst_i32(static_cast<int32_t>(length));
            Value* name_key = (name_key_val != 0xFFFFFFFFu && lowering)
                ? lowering->get_key_id(b, name_key_val)
                : b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
            Value* fn_flags = b.build_iconst_i32(static_cast<int32_t>(fn_flags_val));
            Value* slot_cell = b.build_iconst_i64(0);
            res_val = b.build_call("bronze_function_singleton", Type::i64(), {
                code_addr,
                arity_val,
                length_val,
                name_key,
                fn_flags,
                slot_cell
            });
            break;
        }

        case BronzeOp::CreateFunc: {
            std::string callee = lowering ? lowering->resolve_create_func_callee(inst_ast.callee_name) : inst_ast.callee_name;
            Value* code_addr = b.build_func_addr("__wrapper_" + callee);
            Value* arity_val = b.build_iconst_i32(static_cast<int32_t>(inst_ast.param_count));
            Value* env_val = ensure_type(get_opd(0), Type::i64());
            uint32_t length = 0;
            uint32_t name_key_val = 0xFFFFFFFFu;
            uint32_t fn_flags_val = 0x03;
            if (lowering) {
                auto it_meta = lowering->options().function_meta.find(callee);
                if (it_meta != lowering->options().function_meta.end()) {
                    fn_flags_val = it_meta->second.fn_flags;
                    name_key_val = it_meta->second.name_key;
                    length = it_meta->second.required_args;
                }
            }
            Value* length_val = b.build_iconst_i32(static_cast<int32_t>(length));
            Value* name_key = (name_key_val != 0xFFFFFFFFu && lowering)
                ? lowering->get_key_id(b, name_key_val)
                : b.build_iconst_i32(static_cast<int32_t>(0xFFFFFFFFu));
            Value* fn_flags = b.build_iconst_i32(static_cast<int32_t>(fn_flags_val));
            res_val = b.build_call("bronze_create_function", Type::i64(), {
                code_addr,
                arity_val,
                length_val,
                name_key,
                fn_flags,
                env_val
            });
            break;
        }

        default:
            return false;
    }

    return true;
}

} // namespace il2mir

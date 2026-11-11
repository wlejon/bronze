// Speculated `Math` builtin calls (`Math.cos(x)`, `const cos = Math.cos; cos(x)`):
// unboxed f64 arguments bypass Number boxing and range checks on the fast path,
// and repeated dynamic calls through one SSA callee share its function-identity
// test across dominated blocks.

#include "il_lowering_calls.h"
#include "il_lowering.h"
#include "il_abi.h"
#include "il_lowering_ops.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace il2mir {

Value* build_call_dynamic(Builder& b, const std::vector<Value*>& dyn_args, IlLowering* lowering) {
    size_t argc = dyn_args.size() > 2 ? dyn_args.size() - 2 : 0;
    if (argc <= 16) {
        std::string helper = "bronze_call_dynamic_" + std::to_string(argc);
        return b.build_call(helper, Type::i64(), Span<Value* const>(dyn_args.data(), dyn_args.size()));
    }
    Value* argv = lowering->stage_argv(b, std::vector<Value*>(dyn_args.begin() + 2, dyn_args.end()));
    Value* argc_val = b.build_iconst_i32(static_cast<int32_t>(argc));
    return b.build_call("bronze_call_dynamic_n", Type::i64(), {dyn_args[0], dyn_args[1], argc_val, argv});
}

// An argument as the Value the uniform convention passes, given the
// parameter type it has on a typed edge (`i64` where it is boxed already): an
// f64 NaN-canonicalized (ensure_type), a bool — held in an i32 like every IL
// bool — as the boolean tag. ensure_type alone would hand a bool's 0/1 over as
// raw bits, which as a Value is a denormal double; the IL type, not the MIR
// one, says which it is, since an IL i32 is held in an i32 too.
Value* box_call_arg(IlLowering* lowering, Builder& b, Value* v, Type as) {
    if (as == Type::i8()) {
        Value* bit = b.build_and(b.build_zext_i64(v->type() == Type::i64() ? b.build_trunc_i32(v) : v),
                                 b.build_iconst_i64(1));
        return b.build_or(bit, b.build_iconst_i64(static_cast<int64_t>(kBoolTag)));
    }
    return lowering ? lowering->ensure_type(v, Type::i64(), b) : v;
}

namespace {

// The Math members a method call can speculate on (docs/il2mir.md §7): the
// function object's code pointer, which a moving GC never changes, and the
// machine op (or, for sin and cos, the runtime's f64 kernel, the one the
// builtin itself calls) that computes the member exactly for a Number
// argument. Max and Min take two to four (the shapes three.js calls them in),
// each a Number: 21.3.2.24/25 over them is a fold of a select chain, NaN
// first and -0 below +0.
//
// `Kernel1` and `Kernel2` are the libm members (exp, log, tan, asin, acos,
// atan; atan2): the fast path calls the f64 kernel the function object's own
// code calls (builtin_math.cpp), by name.
enum class BuiltinOp { None, Sqrt, Abs, Floor, Ceil, Sin, Cos, Max, Min, Kernel1, Kernel2 };

bool builtin_takes(BuiltinOp op, uint32_t argc) {
    if (op == BuiltinOp::Max || op == BuiltinOp::Min) return argc >= 2 && argc <= 4;
    if (op == BuiltinOp::Kernel2) return argc == 2;
    return argc == 1;
}

struct BuiltinTarget {
    BuiltinOp op = BuiltinOp::None;
    bronze_fn_code code = nullptr;
    // The builtin's symbol in the shared runtime: an image (an AOT module, an
    // executable) compares against its address as an import, which the
    // loader binds to the function itself (an Abs64 to an import is the
    // symbol's address, not its PLT stub).
    const char* symbol = nullptr;
    // Kernel1 / Kernel2: the f64 kernel's symbol.
    const char* kernel = nullptr;
};

// The builtin a call of a function read with key `key_index` speculates on.
BuiltinTarget builtin_target_for_key(const IlLowering* lowering, uint32_t key_index) {
    if (!lowering) return {};
    const PropertyLoweringHelper& pl = const_cast<IlLowering*>(lowering)->prop_lowering();
    // The hit test compares the method's code word with the builtin's
    // address: this process's, for a module that runs here under the tiered
    // pipeline; the import's, for an image (direct_js_calls), whose loader
    // binds it to the same function.
    const bool here = pl.feedback_driven() && pl.in_process() && pl.enable_inlined_fastpaths();
    const bool image = !pl.in_process() && lowering->options().direct_js_calls;
    if (!here && !image) return {};
    const auto& keys = lowering->options().key_constants;
    if (key_index >= keys.size()) return {};
    const std::string& key = keys[key_index];
    BuiltinTarget t;
    if (key == "sqrt") t = {BuiltinOp::Sqrt, &bronze_math_sqrt, "bronze_math_sqrt"};
    else if (key == "abs") t = {BuiltinOp::Abs, &bronze_math_abs, "bronze_math_abs"};
    else if (key == "floor") t = {BuiltinOp::Floor, &bronze_math_floor, "bronze_math_floor"};
    else if (key == "ceil") t = {BuiltinOp::Ceil, &bronze_math_ceil, "bronze_math_ceil"};
    else if (key == "sin") t = {BuiltinOp::Sin, &bronze_math_sin, "bronze_math_sin"};
    else if (key == "cos") t = {BuiltinOp::Cos, &bronze_math_cos, "bronze_math_cos"};
    else if (key == "max") t = {BuiltinOp::Max, &bronze_math_max, "bronze_math_max"};
    else if (key == "min") t = {BuiltinOp::Min, &bronze_math_min, "bronze_math_min"};
    else if (key == "round") t = {BuiltinOp::Kernel1, &bronze_math_round, "bronze_math_round", "bronze_math_round_f64"};
    else if (key == "exp") t = {BuiltinOp::Kernel1, &bronze_math_exp, "bronze_math_exp", "bronze_math_exp_f64"};
    else if (key == "log") t = {BuiltinOp::Kernel1, &bronze_math_log, "bronze_math_log", "bronze_math_log_f64"};
    else if (key == "tan") t = {BuiltinOp::Kernel1, &bronze_math_tan, "bronze_math_tan", "bronze_math_tan_f64"};
    else if (key == "asin") t = {BuiltinOp::Kernel1, &bronze_math_asin, "bronze_math_asin", "bronze_math_asin_f64"};
    else if (key == "acos") t = {BuiltinOp::Kernel1, &bronze_math_acos, "bronze_math_acos", "bronze_math_acos_f64"};
    else if (key == "atan") t = {BuiltinOp::Kernel1, &bronze_math_atan, "bronze_math_atan", "bronze_math_atan_f64"};
    else if (key == "atan2") t = {BuiltinOp::Kernel2, &bronze_math_atan2, "bronze_math_atan2", "bronze_math_atan2_f64"};
    else return {};
    if (image) t.code = nullptr;
    return t;
}

BuiltinTarget builtin_target(const IlLowering* lowering, const BronzeInstruction& inst) {
    if (!lowering || inst.param_count == 0 || inst.param_count > 4) return {};
    BuiltinTarget t;
    if (inst.op == BronzeOp::MethodCall) {
        if (inst.ic_index == BronzeInstruction::kNoIcIndex) return {};
        t = builtin_target_for_key(lowering, inst.index);
    } else if (inst.op == BronzeOp::CallDynamic && !inst.operands.empty()) {
        // `const cos = Math.cos; cos(x)`: the callee is a value a `prop.get`
        // of the builtin's key read (three.js's setFromEuler, makeRotation*).
        const uint32_t key_index = lowering->prop_get_key_of(inst.operands[0]);
        if (key_index != UINT32_MAX) t = builtin_target_for_key(lowering, key_index);
    }
    if (t.op == BuiltinOp::None || !builtin_takes(t.op, inst.param_count)) return {};
    return t;
}

Value* build_min_max(Builder& b, BuiltinOp op, Value* x, Value* y) {
    return build_min_max_f64(b, op == BuiltinOp::Max, x, y);
}

struct BuiltinArg {
    uint32_t id = UINT32_MAX;
    Type kind = Type::i64();
    Value* f64 = nullptr;
    Value* boxed = nullptr;
};

// Tests whether `method` is the function object for `target`: Object tag,
// Function header kind, and matching code word. A non-object's address is
// redirected to a zeroed block (in-process) or the pinned TLS block (image) so
// both header loads stay branch-free.
Value* build_builtin_fn_check(Builder& b, Value* method, const BuiltinTarget& target) {
    constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
    constexpr uint64_t kObjectTagBits = 0xFFF1000000000000ULL;
    constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
    constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | 0xFFF1;
    alignas(16) static const uint64_t kNotAnObject[4] = {};

    Value* is_obj = b.build_eq(b.build_and(method, b.build_iconst_i64(static_cast<int64_t>(kTagMask))),
                               b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* ptr = b.build_and(method, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    Value* not_object = target.code != nullptr
        ? b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(&kNotAnObject[0])))
        : b.build_pinned_tls_read();
    Value* base = b.build_select(is_obj, ptr, not_object);
    Value* is_fn = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kFunctionHeaderLow));
    Value* is_target = target.code != nullptr
        ? b.build_eq(b.build_load(Type::i64(), base, BRONZE_ABI_FN_CODE_OFFSET),
                     b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(target.code))))
        : b.build_eq(b.build_load(Type::ptr(), base, BRONZE_ABI_FN_CODE_OFFSET), b.build_func_addr(target.symbol));
    return b.build_and(b.build_and(is_obj, is_fn), is_target);
}

// Lowers a speculated Math builtin call. Arguments proven to be unboxed f64
// skip the `<= kBronzeNumberMaxBits` check on the fast path and are boxed only
// inside `slow`; dynamic arguments are unboxed/tested in `cur` and bitcast in
// `fast`. When `callee_id != UINT32_MAX` (`CallDynamic`), the callee-identity
// boolean `fn_ok` is memoized across dominated continuation blocks.
Value* lower_builtin_call_impl(IlLowering* lowering, const BuiltinTarget& target, uint32_t key_index, Builder& b,
                               uint32_t callee_id, Value* method_preloaded, uint32_t this_id,
                               const std::vector<BuiltinArg>& raw_args,
                               const std::unordered_map<uint32_t, Value*>& val_map,
                               Value* lookup_fn_ok) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    const std::string_view tag = pl.key_tag(key_index);
    const std::optional<uint32_t> prof_misses = lowering->spec().peek_profile(SpecKind::CallTarget, tag);
    const bool slow_cold = !prof_misses || *prof_misses == 0;

    BasicBlock* cur = b.current_block();
    const std::string prefix = "builtin_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(prefix + "_fast");
    BasicBlock* slow = b.append_block((slow_cold && !prof_misses) ? (prefix + "_slow_cold") : (prefix + "_slow"));
    if (slow_cold) slow->set_cold();
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    Value* fn_ok = lookup_fn_ok;
    Value* method_cur = method_preloaded;
    if (!fn_ok) {
        if (!method_cur) {
            method_cur = lowering->ensure_type(lowering->get_val_by_id(callee_id, b, val_map), Type::i64(), b);
        }
        fn_ok = build_builtin_fn_check(b, method_cur, target);
    }

    std::vector<BuiltinArg> args = raw_args;
    Value* is_num = nullptr;
    for (BuiltinArg& arg : args) {
        if (arg.f64 != nullptr) continue;
        Value* raw = lowering->get_val_by_id(arg.id, b, val_map);
        arg.boxed = box_call_arg(lowering, b, raw, arg.kind);
        Value* n = b.build_ule(arg.boxed, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
        is_num = is_num ? b.build_and(is_num, n) : n;
    }
    Value* hit = is_num ? b.build_and(fn_ok, is_num) : fn_ok;
    lowering->spec().emit_branch(b, hit, fast, slow, SpecKind::CallTarget, tag);

    // `emit_branch` positioned `b` at the end of `slow`: materialize any
    // values the fast path did not need (callee when `fn_ok` came from cache,
    // `this` reloaded from its GC-tracked root, and boxed forms of f64 args).
    Value* slow_method = method_cur
        ? method_cur
        : lowering->ensure_type(lowering->get_val_by_id(callee_id, b, val_map), Type::i64(), b);
    Value* slow_this = lowering->ensure_type(lowering->get_val_by_id(this_id, b, val_map), Type::i64(), b);
    std::vector<Value*> dyn_args;
    dyn_args.reserve(2 + args.size());
    dyn_args.push_back(slow_method);
    dyn_args.push_back(slow_this);
    for (const BuiltinArg& arg : args) {
        dyn_args.push_back(arg.f64 != nullptr ? box_call_arg(lowering, b, arg.f64, Type::f64()) : arg.boxed);
    }
    b.build_br(merge, {build_call_dynamic(b, dyn_args, lowering)});

    b.position_at_end(fast);
    auto arg_as_f64 = [&](size_t idx) -> Value* {
        return args[idx].f64 != nullptr ? args[idx].f64 : b.build_bitcast_f64_i64(args[idx].boxed);
    };
    Value* x = arg_as_f64(0);
    if (target.op == BuiltinOp::Max || target.op == BuiltinOp::Min) {
        for (size_t i = 1; i < args.size(); ++i) x = build_min_max(b, target.op, x, arg_as_f64(i));
    }
    Value* r = target.op == BuiltinOp::Max || target.op == BuiltinOp::Min ? x
             : target.op == BuiltinOp::Kernel1 ? b.build_call(target.kernel, Type::f64(), {x})
             : target.op == BuiltinOp::Kernel2 ? b.build_call(target.kernel, Type::f64(), {x, arg_as_f64(1)})
             : target.op == BuiltinOp::Sqrt  ? b.build_sqrt_f64(x)
             : target.op == BuiltinOp::Abs   ? b.build_fabs(x)
             : target.op == BuiltinOp::Floor ? b.build_floor_f64(x)
             : target.op == BuiltinOp::Ceil  ? b.build_ceil_f64(x)
             : target.op == BuiltinOp::Sin   ? b.build_call("bronze_math_sin_f64", Type::f64(), {x})
                                             : b.build_call("bronze_math_cos_f64", Type::f64(), {x});
    b.build_br(merge, {box_f64_bits(b, r)});

    b.position_at_end(merge);
    return result;
}

} // namespace

// 21.3.2.24 Math.max / 21.3.2.25 Math.min of two Numbers, as the runtime's
// bronze_math_max2_f64 / bronze_math_min2_f64 answer it: a NaN operand is the
// answer, two zeros are -0 only when both are (max) or either is (min) — which
// is what `x + y` and `-(-x + -y)` give for zeros — and otherwise the ordered
// compare picks (equal non-zeros are one value). A NaN result stays the result
// of every later step, so a fold of it over more operands is the variadic one.
Value* build_min_max_f64(Builder& b, bool max, Value* x, Value* y) {
    Value* pick = b.build_select(max ? b.build_slt(x, y) : b.build_slt(y, x), y, x);
    Value* zeros = max ? b.build_add(x, y) : b.build_neg(b.build_add(b.build_neg(x), b.build_neg(y)));
    Value* both_zero = b.build_and(b.build_eq(x, y), b.build_eq(x, b.build_fconst_f64(0.0)));
    Value* r = b.build_select(both_zero, zeros, pick);
    // x NaN: the compares above are all false, so `r` is x already.
    return b.build_select(b.build_ne(y, y), y, r);
}

bool method_call_is_speculated_builtin(const IlLowering* lowering, const BronzeInstruction& inst) {
    return builtin_target(lowering, inst).op != BuiltinOp::None;
}

void IlLowering::begin_builtin_lowering(const BronzeFunction& fn_ast) {
    box_f64_src_.clear();
    builtin_fn_guards_.clear();
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::Box && inst.box_type == BronzeType::F64 &&
                inst.result_id != UINT32_MAX && !inst.operands.empty()) {
                box_f64_src_.emplace(inst.result_id, inst.operands[0]);
            }
        }
    }
}

Value* IlLowering::peek_unboxed_f64(uint32_t id, const std::unordered_map<uint32_t, Value*>& val_map) const {
    for (int depth = 0; depth < 8; ++depth) {
        auto it = box_f64_src_.find(id);
        if (it == box_f64_src_.end()) break;
        id = it->second;
    }
    if (module_env_regs_.count(id) != 0 || number_bits_ids_.count(id) != 0) return nullptr;
    auto it = val_map.find(id);
    if (it == val_map.end() || !it->second) return nullptr;
    Value* v = it->second;
    if (v->type() == Type::f64()) return v;

    if (v->type().is_tagged()) {
        const Instruction* tag = v->is_instruction() ? v->defining_instruction() : nullptr;
        if (!tag || tag->opcode() != Opcode::bitcast_tagged_i64 || tag->operand_count() != 1) return nullptr;
        v = tag->operand(0);
    }
    if (!v || v->type() != Type::i64() || !v->is_instruction()) return nullptr;
    const Instruction* sel = v->defining_instruction();
    if (!sel || sel->opcode() != Opcode::select || sel->operand_count() != 3) return nullptr;
    const Instruction* nan = sel->operand(1) && sel->operand(1)->is_instruction()
        ? sel->operand(1)->defining_instruction()
        : nullptr;
    if (!nan || nan->opcode() != Opcode::iconst_i64 ||
        static_cast<uint64_t>(nan->imm_i64()) != BRONZE_ABI_CANONICAL_NAN_BITS) {
        return nullptr;
    }
    const Instruction* bits = sel->operand(2) && sel->operand(2)->is_instruction()
        ? sel->operand(2)->defining_instruction()
        : nullptr;
    if (!bits || bits->opcode() != Opcode::bitcast_i64_f64 || bits->operand_count() != 1) return nullptr;
    Value* f = bits->operand(0);
    const Instruction* cmp = sel->operand(0) && sel->operand(0)->is_instruction()
        ? sel->operand(0)->defining_instruction()
        : nullptr;
    if (!f || f->type() != Type::f64() || !cmp || cmp->opcode() != Opcode::ne || cmp->operand_count() != 2 ||
        cmp->operand(0) != f || cmp->operand(1) != f) {
        return nullptr;
    }
    return f;
}

Value* IlLowering::lookup_builtin_fn_guard(uint32_t callee_id, uint32_t key_index, BasicBlock* cur) const {
    if (!cur || !cur->parent() || coro_frame_val_ != nullptr || module_env_regs_.count(callee_id) != 0) {
        return nullptr;
    }
    const Function* fn = cur->parent();
    const auto& blocks = fn->blocks();
    if (blocks.size() > 4096) return nullptr;
    const size_t upfront = std::max(
        current_fn_ast() ? current_fn_ast()->blocks.size() : size_t{0},
        hoist_block_ ? static_cast<size_t>(hoist_block_->id()) + 1 : size_t{0});

    auto reaches_cur_internal = [&](const std::vector<BasicBlock*>& roots, const BasicBlock* skip_bb) -> bool {
        std::vector<uint8_t> seen(blocks.size(), 0);
        std::vector<BasicBlock*> work;
        work.reserve(roots.size());
        for (BasicBlock* r : roots) {
            if (!r || r == skip_bb) continue;
            if (r == cur) return true;
            if (r->id() < seen.size() && !seen[r->id()]) {
                seen[r->id()] = 1;
                work.push_back(r);
            }
        }
        auto visit = [&](BasicBlock* tgt) -> bool {
            if (!tgt || tgt == skip_bb || static_cast<size_t>(tgt->id()) < upfront) return false;
            if (tgt == cur) return true;
            if (tgt->id() < seen.size() && !seen[tgt->id()]) {
                seen[tgt->id()] = 1;
                work.push_back(tgt);
            }
            return false;
        };
        while (!work.empty()) {
            BasicBlock* u = work.back();
            work.pop_back();
            const Instruction* term = u->terminator();
            if (!term) continue;
            if (visit(term->branch_target().block) ||
                visit(term->true_target().block) ||
                visit(term->false_target().block)) {
                return true;
            }
            for (const auto& sc : term->switch_cases()) {
                if (visit(sc.target.block)) return true;
            }
        }
        return false;
    };

    for (BasicBlock* bb : blocks) {
        if (bb && static_cast<size_t>(bb->id()) >= upfront && bb != cur && !bb->terminator()) return nullptr;
    }

    for (const BuiltinFnGuard& g : builtin_fn_guards_) {
        if (g.callee_id != callee_id || g.key_index != key_index || g.fn != fn || !g.def_bb || !g.fn_ok) continue;
        if (g.def_bb == cur) return g.fn_ok;
        if (static_cast<size_t>(cur->id()) < upfront) continue;
        if (!reaches_cur_internal({g.def_bb}, nullptr)) continue;
        std::vector<BasicBlock*> other_upfront;
        other_upfront.reserve(upfront);
        for (BasicBlock* bb : blocks) {
            if (bb && static_cast<size_t>(bb->id()) < upfront && bb != g.def_bb) other_upfront.push_back(bb);
        }
        if (!reaches_cur_internal(other_upfront, g.def_bb)) return g.fn_ok;
    }
    return nullptr;
}

void IlLowering::note_builtin_fn_guard(uint32_t callee_id, uint32_t key_index, BasicBlock* def_bb, Value* fn_ok) {
    if (!def_bb || !def_bb->parent() || !fn_ok || coro_frame_val_ != nullptr ||
        module_env_regs_.count(callee_id) != 0) {
        return;
    }
    builtin_fn_guards_.push_back({callee_id, key_index, def_bb->parent(), def_bb, fn_ok});
}

Value* IlLowering::try_lower_builtin_method_call(const BronzeInstruction& inst, Builder& b, Value* recv,
                                                 const std::vector<Type>& arg_kinds,
                                                 const std::unordered_map<uint32_t, Value*>& val_map,
                                                 Value* site) {
    const BuiltinTarget target = builtin_target(this, inst);
    if (target.op == BuiltinOp::None || !site || inst.operands.size() < 1 + inst.param_count) return nullptr;

    std::vector<BuiltinArg> args;
    args.reserve(inst.param_count);
    for (uint32_t a = 0; a < inst.param_count; ++a) {
        const uint32_t arg_id = inst.operands[1 + a];
        const Type kind = a < arg_kinds.size() ? arg_kinds[a] : Type::i64();
        Value* f64 = (kind == Type::f64() || kind == Type::i64()) ? peek_unboxed_f64(arg_id, val_map) : nullptr;
        args.push_back({arg_id, kind, f64, nullptr});
    }

    // A method site read as a property keeps no packed row (IcSite::packed).
    IcSite method_site = prop_lowering_.ic_site_ref(b, inst.ic_index);
    method_site.packed = false;
    Value* method = prop_lowering_.lower_prop_get_mono(b, recv, inst.index, method_site);
    return lower_builtin_call_impl(this, target, inst.index, b, UINT32_MAX, method, inst.operands[0], args,
                                   val_map, nullptr);
}

Value* IlLowering::try_lower_builtin_dynamic_call(const BronzeInstruction& inst, Builder& b,
                                                  const std::unordered_map<uint32_t, Value*>& val_map) {
    if (run_once() || inst.operands.size() < 2 + inst.param_count) return nullptr;
    const BuiltinTarget target = builtin_target(this, inst);
    if (target.op == BuiltinOp::None) return nullptr;

    const uint32_t callee_id = inst.operands[0];
    const uint32_t this_id = inst.operands[1];
    const uint32_t key_index = prop_get_key_of(callee_id);
    BasicBlock* cur = b.current_block();
    Value* cached_fn_ok = lookup_builtin_fn_guard(callee_id, key_index, cur);

    std::vector<BuiltinArg> args;
    args.reserve(inst.param_count);
    for (uint32_t a = 0; a < inst.param_count; ++a) {
        const uint32_t arg_id = inst.operands[2 + a];
        args.push_back({arg_id, Type::i64(), peek_unboxed_f64(arg_id, val_map), nullptr});
    }

    Value* method_cur = nullptr;
    Value* fn_ok = cached_fn_ok;
    if (!fn_ok) {
        b.position_at_end(cur);
        method_cur = ensure_type(get_val_by_id(callee_id, b, val_map), Type::i64(), b);
        fn_ok = build_builtin_fn_check(b, method_cur, target);
        note_builtin_fn_guard(callee_id, key_index, cur, fn_ok);
    }
    return lower_builtin_call_impl(this, target, key_index, b, callee_id, method_cur, this_id, args, val_map,
                                   fn_ok);
}

} // namespace il2mir

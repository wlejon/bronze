#include "il_property_lowering.h"
#include "il_lowering.h"
#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

namespace il2mir {

// The inline-cache paths: il_ic_lowering.cpp. lower_elem_get /
// lower_elem_set: il_elem_lowering.cpp.

bool is_property_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::PropGet:
        case BronzeOp::PropSet:
        case BronzeOp::PropDelete:
        case BronzeOp::MethodDef:
        case BronzeOp::MethodDefComputed:
        case BronzeOp::DefineOwnAttr:
        case BronzeOp::AccessorDef:
        case BronzeOp::AccessorDefComputed:
        case BronzeOp::ElemGet:
        case BronzeOp::ElemGetTyped:
        case BronzeOp::ElemSet:
        case BronzeOp::ElemSetTyped:
        case BronzeOp::ElemDelete:
            return true;
        default:
            return false;
    }
}

bool lower_property_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* /*fn*/,
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
        if (lowering) {
            return lowering->ensure_type(val, target_type, b);
        }
        return val;
    };

    auto get_key_id = [&](uint32_t key_idx) -> Value* {
        if (lowering) {
            return lowering->get_key_id(b, key_idx);
        }
        return b.build_iconst_i32(static_cast<int32_t>(key_idx));
    };

    PropertyLoweringHelper& prop_lowering = lowering->prop_lowering();

    switch (inst_ast.op) {
        case BronzeOp::PropGet: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            const IcSite site = prop_lowering.ic_site_ref(b, inst_ast.ic_index);
            // A site the IL calls monomorphic gets the way-0 guard-and-load
            // in front of the helper: one compare against the most recently
            // installed shape (the helper fills move-to-front), never a
            // chain over the other ways. Every site could carry it — the
            // guard is correct for any receiver — but each one is a merge
            // GVN-PRE re-walks the function for, and on three.js putting it
            // at every read doubled the optimized compile for a gain the
            // `mono` sites alone already deliver. A program that runs under
            // the tiered pipeline with speculation feedback inlines every
            // site: its misses, not inference, decide what tier 2 keeps, and
            // only hot functions are optimized at all. Code that runs once
            // (IlLowering::run_once) calls the helper, which caches as well.
            const bool mono = inst_ast.is_mono || prop_lowering.feedback_driven();
            if (site && mono && prop_lowering.enable_inlined_fastpaths() &&
                inst_ast.index != PropertyLoweringHelper::kNoKey && !lowering->run_once()) {
                res_val = prop_lowering.lower_prop_get_mono(b, obj_val, inst_ast.index, site);
                return true;
            }
            res_val = prop_lowering.lower_prop_get(b, obj_val, inst_ast.index,
                                                   site ? prop_lowering.site_ptr(b, site) : nullptr);
            return true;
        }

        case BronzeOp::PropSet: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* val = ensure_type(get_opd(1), Type::i64());
            const IcSite site = prop_lowering.ic_site_ref(b, inst_ast.ic_index);
            const uint32_t strict = static_cast<uint32_t>(inst_ast.imm_i64);
            // The same sites the read inlines (see PropGet), less the writes
            // to an object this function just created: those add a property
            // (a shape transition), which the inline store never does.
            const bool mono = inst_ast.is_mono || prop_lowering.feedback_driven();
            if (site && mono && prop_lowering.enable_inlined_fastpaths() &&
                prop_lowering.inline_sets() && inst_ast.index != PropertyLoweringHelper::kNoKey &&
                !lowering->run_once() && !lowering->is_fresh_object(inst_ast.operands[0])) {
                prop_lowering.lower_prop_set_mono(b, obj_val, inst_ast.index, val, strict, site);
                return true;
            }
            prop_lowering.lower_prop_set(b, obj_val, inst_ast.index, val, strict,
                                         site ? prop_lowering.site_ptr(b, site) : nullptr);
            return true;
        }

        case BronzeOp::PropDelete: {
            Value* target = ensure_type(get_opd(0), Type::i64());
            Value* key_id = get_key_id(inst_ast.index);
            Value* strict = b.build_iconst_i32(inst_ast.imm_i64 != 0 ? 1 : 0);
            res_val = b.build_and(b.build_call("bronze_prop_delete", Type::i32(), {target, key_id, strict}), b.build_iconst_i32(1));
            return true;
        }

        case BronzeOp::MethodDef: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* closure_val = ensure_type(get_opd(1), Type::i64());
            prop_lowering.lower_method_def(b, obj_val, inst_ast.index, closure_val);
            return true;
        }

        case BronzeOp::MethodDefComputed: {
            Value* target = ensure_type(get_opd(0), Type::i64());
            Value* key = ensure_type(get_opd(1), Type::i64());
            Value* closure_val = ensure_type(get_opd(2), Type::i64());
            b.build_call("bronze_method_def_computed", Type::void_type(), {target, key, closure_val});
            return true;
        }

        case BronzeOp::DefineOwnAttr: {
            Value* target = ensure_type(get_opd(0), Type::i64());
            Value* value = ensure_type(get_opd(1), Type::i64());
            Value* key_id = get_key_id(inst_ast.index);
            Value* mask = b.build_iconst_i32(static_cast<int32_t>(inst_ast.imm_i64));
            b.build_call("bronze_define_own_attr", Type::void_type(), {target, key_id, value, mask});
            return true;
        }

        case BronzeOp::AccessorDef: {
            Value* target = ensure_type(get_opd(0), Type::i64());
            Value* key_id = get_key_id(inst_ast.index);
            Value* getter = ensure_type(get_opd(1), Type::i64());
            Value* setter = ensure_type(get_opd(2), Type::i64());
            Value* enum_val = b.build_iconst_i32(inst_ast.imm_bool ? 1 : 0);
            b.build_call("bronze_accessor_def", Type::void_type(), {target, key_id, getter, setter, enum_val});
            return true;
        }

        case BronzeOp::AccessorDefComputed: {
            Value* target = ensure_type(get_opd(0), Type::i64());
            Value* key = ensure_type(get_opd(1), Type::i64());
            Value* getter = ensure_type(get_opd(2), Type::i64());
            Value* setter = ensure_type(get_opd(3), Type::i64());
            Value* enum_val = b.build_iconst_i32(inst_ast.imm_bool ? 1 : 0);
            b.build_call("bronze_accessor_def_computed", Type::void_type(), {target, key, getter, setter, enum_val});
            return true;
        }

        case BronzeOp::ElemGet:
        case BronzeOp::ElemGetTyped: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* idx_val = get_opd(1);
            if (!idx_val) return false;
            res_val = lowering->run_once() ? prop_lowering.lower_elem_get_call(b, obj_val, idx_val)
                                           : prop_lowering.lower_elem_get(b, obj_val, idx_val);
            if (inst_ast.op == BronzeOp::ElemGetTyped && inst_ast.result_type == BronzeType::F64) {
                res_val = ensure_type(res_val, Type::f64());
            }
            return true;
        }

        case BronzeOp::ElemSet:
        case BronzeOp::ElemSetTyped: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* idx_val = get_opd(1);
            if (!idx_val) return false;
            Value* val = ensure_type(get_opd(2), Type::i64());
            if (lowering->run_once()) {
                prop_lowering.lower_elem_set_call(b, obj_val, idx_val, val, inst_ast.index);
            } else {
                prop_lowering.lower_elem_set(b, obj_val, idx_val, val, inst_ast.index);
            }
            return true;
        }

        case BronzeOp::ElemDelete: {
            Value* target = ensure_type(get_opd(0), Type::i64());
            Value* index = ensure_type(get_opd(1), Type::i64());
            Value* strict = b.build_iconst_i32(inst_ast.imm_i64 != 0 ? 1 : 0);
            res_val = b.build_and(b.build_call("bronze_elem_delete", Type::i32(), {target, index, strict}), b.build_iconst_i32(1));
            return true;
        }

        default:
            return false;
    }
}

} // namespace il2mir

#include "il_property_lowering.h"
#include "il_lowering.h"
#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

#include <cstdint>
#include <string>

namespace il2mir {

// The inline-cache paths: il_ic_lowering.cpp. lower_elem_get /
// lower_elem_set: il_elem_lowering.cpp.

bool is_property_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::PropGet:
        case BronzeOp::PropSet:
        case BronzeOp::PropDefine:
        case BronzeOp::PropDelete:
        case BronzeOp::MethodDef:
        case BronzeOp::MethodDefComputed:
        case BronzeOp::DefineOwnAttr:
        case BronzeOp::AccessorDef:
        case BronzeOp::AccessorDefComputed:
        case BronzeOp::ElemGet:
        case BronzeOp::ElemSet:
        case BronzeOp::ElemDelete:
            return true;
        default:
            return false;
    }
}

namespace {

// Whether key constant `key_index` is a canonical array index below 2^31
// ("0", "17"; not "01" or "-1"): a property named by one is the element
// `o[k]` reads and writes, whatever the receiver.
bool index_key(const IlLowering* lowering, uint32_t key_index, int32_t& out) {
    const auto& keys = lowering->options().key_constants;
    if (key_index >= keys.size()) return false;
    const std::string& k = keys[key_index];
    if (k.empty() || k.size() > 10 || (k.size() > 1 && k[0] == '0')) return false;
    int64_t v = 0;
    for (char c : k) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v > INT32_MAX) return false;
    out = static_cast<int32_t>(v);
    return true;
}

bool length_key(const IlLowering* lowering, uint32_t key_index) {
    const auto& keys = lowering->options().key_constants;
    return key_index < keys.size() && keys[key_index] == "length";
}

}  // namespace

bool lower_property_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* /*fn*/,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
) {
    auto get_opd = [&](size_t idx) -> Value* {
        if (idx < inst_ast.operands.size()) return lowering->get_val_by_id(inst_ast.operands[idx], b, val_map);
        return nullptr;
    };
    auto ensure_type = [&](Value* val, Type target_type) -> Value* {
        return lowering->ensure_type(val, target_type, b);
    };
    auto get_key_id = [&](uint32_t key_idx) -> Value* { return lowering->get_key_id(b, key_idx); };

    PropertyLoweringHelper& prop_lowering = lowering->prop_lowering();

    switch (inst_ast.op) {
        // The site's way-0 guard and load in front of bronze_prop_get, or the
        // helper alone for a read with no cache.
        case BronzeOp::PropGet: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            // `a[0]`: an index-named read is the element read, which no
            // shape way can answer.
            if (int32_t k = 0; index_key(lowering, inst_ast.index, k)) {
                res_val = prop_lowering.lower_elem_get(b, obj_val, b.build_iconst_i32(k));
                return true;
            }
            const IcSite site = prop_lowering.ic_site_ref(b, inst_ast.ic_index);
            // A cold body's read is the helper alone (cold_code).
            const bool cold = prop_lowering.cold_code();
            if (site && length_key(lowering, inst_ast.index) && !cold) {
                res_val = prop_lowering.lower_length_get(b, obj_val, inst_ast.index, site);
            } else if (site && inst_ast.index != PropertyLoweringHelper::kNoKey && !cold) {
                res_val = prop_lowering.lower_prop_get_mono(b, obj_val, inst_ast.index, site);
            } else {
                res_val = prop_lowering.lower_prop_get(b, obj_val, inst_ast.index, site);
            }
            return true;
        }

        // An assignment, or a DEFINITION (il_ast.h PropDefine): the site's
        // way-0 store in front of the helper, which for a definition is
        // bronze_prop_define (lower_prop_set reads the mode).
        case BronzeOp::PropSet:
        case BronzeOp::PropDefine: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* val = ensure_type(get_opd(1), Type::i64());
            const IcSite site = prop_lowering.ic_site_ref(b, inst_ast.ic_index);
            // The strict flag of an assignment, or kDefineWriteMode for a
            // PropDefine: the helper the miss calls is chosen by it.
            const uint32_t strict = static_cast<uint32_t>(inst_ast.imm_i64);
            // An index-named assignment is the element write. An array
            // literal's element (kLiteralElementWriteMode) and any other
            // index-named definition stay bronze_prop_define's, which
            // defines rather than assigns.
            if (int32_t k = 0; strict < kDefineWriteMode && index_key(lowering, inst_ast.index, k)) {
                prop_lowering.lower_elem_set(b, obj_val, b.build_iconst_i32(k), val, strict);
                return true;
            }
            if (int32_t k = 0; strict == kLiteralElementWriteMode && index_key(lowering, inst_ast.index, k)) {
                prop_lowering.lower_literal_element(b, obj_val, k, inst_ast.index, val,
                                                    site ? prop_lowering.site_ptr(b, site) : nullptr);
                return true;
            }
            if (site && prop_lowering.inline_sets() && inst_ast.index != PropertyLoweringHelper::kNoKey && !prop_lowering.cold_code()) {
                prop_lowering.lower_prop_set_mono(b, obj_val, inst_ast.index, val, strict, site,
                                                  lowering->is_primitive_value(inst_ast.operands[1]));
            } else {
                prop_lowering.lower_prop_set(b, obj_val, inst_ast.index, val, strict,
                                             site ? prop_lowering.site_ptr(b, site) : nullptr);
            }
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
            b.build_call("bronze_define_own_attr", Type::void_type(),
                         {target, key_id, value, b.build_iconst_i32(static_cast<int32_t>(inst_ast.imm_i64)),
                          b.build_iconst_i64(0)});
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

        case BronzeOp::ElemGet: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* idx_val = get_opd(1);
            if (!idx_val) return false;
            res_val = prop_lowering.lower_elem_get(b, obj_val, idx_val);
            return true;
        }

        case BronzeOp::ElemSet: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* idx_val = get_opd(1);
            if (!idx_val) return false;
            Value* val = ensure_type(get_opd(2), Type::i64());
            prop_lowering.lower_elem_set(b, obj_val, idx_val, val, inst_ast.index);
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

#include "il_property_lowering.h"
#include "il_lowering.h"
#include "il_abi.h"
#include "il_property.h"
#include "il/key.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <cstdlib>
#include <unordered_set>

namespace il2mir {

namespace {

bool is_buffer_safe_function(const BronzeFunction& fn_ast,
                             const PropertyLoweringHelper& prop_lowering,
                             const IlLowering& lowering,
                             std::unordered_set<uint32_t>& out_ta_receivers,
                             std::unordered_set<uint32_t>& out_construct_ta_ids) {
    if (fn_ast.blocks.size() <= 1 || fn_ast.is_coroutine_body()) return false;
    std::unordered_set<uint32_t> safe_unbox_ids;
    for (const auto& p : fn_ast.params) {
        if (lowering.is_primitive_value(p.first)) safe_unbox_ids.insert(p.first);
    }
    for (const auto& blk : fn_ast.blocks) {
        if (blk.handler_id != UINT32_MAX) return false;
        for (const auto& bp : blk.params) {
            if (lowering.is_primitive_value(bp.first)) safe_unbox_ids.insert(bp.first);
        }
        for (const auto& inst : blk.instructions) {
            if (inst.result_id != UINT32_MAX && lowering.is_primitive_value(inst.result_id)) {
                safe_unbox_ids.insert(inst.result_id);
            }
            if (inst.op == BronzeOp::PropGet && prop_lowering.key_tag(inst.index) == "length" &&
                inst.result_id != UINT32_MAX) {
                safe_unbox_ids.insert(inst.result_id);
            }
            if ((inst.op == BronzeOp::ElemGetTyped || inst.op == BronzeOp::ElemSetTyped) &&
                !inst.operands.empty()) {
                const int64_t k = inst.imm_i64 & 0xFFFF;
                if (k >= BRONZE_ABI_TA_KIND_INT8 && k <= BRONZE_ABI_TA_KIND_FLOAT64) {
                    out_ta_receivers.insert(inst.operands[0]);
                }
                if (inst.op == BronzeOp::ElemGetTyped && inst.result_id != UINT32_MAX) {
                    safe_unbox_ids.insert(inst.result_id);
                }
            }
        }
    }
    if (out_ta_receivers.empty()) return false;

    for (size_t b_idx = 0; b_idx < fn_ast.blocks.size(); ++b_idx) {
        for (const auto& inst : fn_ast.blocks[b_idx].instructions) {
            switch (inst.op) {
                case BronzeOp::ConstI32:
                case BronzeOp::ConstF64:
                case BronzeOp::ConstBool:
                case BronzeOp::ConstNull:
                case BronzeOp::ConstUndefined:
                case BronzeOp::IsNumber:
                case BronzeOp::IsNullish:
                case BronzeOp::IsDenseArray:
                case BronzeOp::NumTruthy:
                case BronzeOp::Box:
                case BronzeOp::Jump:
                case BronzeOp::Branch:
                case BronzeOp::Ret:
                case BronzeOp::CensusRecord:
                case BronzeOp::KeepAlive:
                case BronzeOp::PinGuard:
                case BronzeOp::GlobalGet:
                case BronzeOp::EnvGet:
                case BronzeOp::EnvGetTdz:
                case BronzeOp::ModuleEnvGet:
                case BronzeOp::CmpLt:
                case BronzeOp::CmpLe:
                case BronzeOp::CmpGt:
                case BronzeOp::CmpGe:
                case BronzeOp::CmpEq:
                case BronzeOp::CmpNe:
                    break;
                case BronzeOp::ElemGetTyped:
                case BronzeOp::ElemSetTyped: {
                    const int64_t k = inst.imm_i64 & 0xFFFF;
                    if (k < BRONZE_ABI_TA_KIND_INT8 || k > BRONZE_ABI_TA_KIND_FLOAT64) return false;
                    break;
                }
                case BronzeOp::Add:
                case BronzeOp::Sub:
                case BronzeOp::Mul:
                case BronzeOp::Div:
                case BronzeOp::Mod:
                case BronzeOp::Pow:
                case BronzeOp::Neg:
                case BronzeOp::BitAnd:
                case BronzeOp::BitOr:
                case BronzeOp::BitXor:
                case BronzeOp::BitNot:
                case BronzeOp::Shl:
                case BronzeOp::Shr:
                case BronzeOp::UShr:
                case BronzeOp::MathImul:
                case BronzeOp::MathUnary:
                    if (inst.result_type != BronzeType::F64 && inst.result_type != BronzeType::I32) {
                        return false;
                    }
                    break;
                case BronzeOp::ToInt32:
                case BronzeOp::Unbox:
                    if (inst.raw_unbox || inst.nullish_unbox) break;
                    if (inst.operands.empty() || safe_unbox_ids.count(inst.operands[0]) == 0) {
                        return false;
                    }
                    break;
                case BronzeOp::PropGet:
                    if (prop_lowering.key_tag(inst.index) != "length") return false;
                    break;
                case BronzeOp::Call:
                    if (inst.callee_name.rfind("__bronze_global_read_k", 0) != 0) return false;
                    break;
                case BronzeOp::Construct:
                    if (inst.operands.size() != 2 || safe_unbox_ids.count(inst.operands[1]) == 0) {
                        return false;
                    }
                    if (inst.result_id != UINT32_MAX) out_construct_ta_ids.insert(inst.result_id);
                    break;
                case BronzeOp::MethodCall:
                    if (prop_lowering.key_tag(inst.index) != "fill" || inst.operands.size() != 2 ||
                        safe_unbox_ids.count(inst.operands[1]) == 0) {
                        return false;
                    }
                    break;
                default:
                    return false;
            }
        }
    }
    return true;
}

// Whether `id` is the result of an environment or module-binding read.
bool defined_by_binding_read(const BronzeFunction& fn_ast, uint32_t id) {
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.result_id != id) continue;
            return inst.op == BronzeOp::EnvGet || inst.op == BronzeOp::ModuleEnvGet;
        }
    }
    return false;
}

bool get_preloaded_ta_cursor(IlLowering* lowering, uint32_t obj_id, Builder& b,
                             const std::unordered_map<uint32_t, Value*>& val_map,
                             Value*& out_data, Value*& out_len) {
    out_data = nullptr;
    out_len = nullptr;
    static const bool unsound_pins = [] {
        const char* env = std::getenv("BRONZE_UNSOUND_PINS");
        return env && env[0] && env[0] != '0';
    }();
    if (!lowering || unsound_pins || !lowering->prop_lowering().enable_inlined_fastpaths()) {
        return false;
    }
    auto& cursors = lowering->ta_cursors();
    if (auto it = cursors.find(obj_id); it != cursors.end()) {
        out_data = it->second.first;
        out_len = it->second.second;
        return out_data != nullptr && out_len != nullptr;
    }
    const BronzeFunction* fn_ast = lowering->current_fn_ast();
    if (!fn_ast) return false;
    auto vit = val_map.find(obj_id);
    if (vit == val_map.end() || !vit->second) return false;
    Value* raw_obj = vit->second;

    std::unordered_set<uint32_t> ta_receivers;
    std::unordered_set<uint32_t> construct_ta_ids;
    const bool fn_safe =
        is_buffer_safe_function(*fn_ast, lowering->prop_lowering(), *lowering, ta_receivers,
                                construct_ta_ids) &&
        ta_receivers.count(obj_id) != 0;
    bool is_fn_param = false;
    if (fn_safe) {
        for (const auto& p : fn_ast->params) {
            if (p.first == obj_id) {
                is_fn_param = true;
                break;
            }
        }
    }
    // A cursor is a raw pointer into the array's storage, held across the
    // whole function, and a collection moves that storage: so it is taken only
    // where nothing after it can collect. A buffer-safe function allocates
    // nowhere but in its typed-array constructions, so the parameter's cursor
    // needs none of them and a constructed array's needs to be the only one
    // (another `new` after it, or a string built in the loop that reads it,
    // left every read and write going to the storage's old address).
    if (construct_ta_ids.size() > (construct_ta_ids.count(obj_id) != 0 ? 1u : 0u)) return false;
    const bool is_construct =
        raw_obj->is_instruction() && fn_safe && construct_ta_ids.count(obj_id) != 0;
    // A captured or module binding read once (the N-body kernel's `pos`, `vel`
    // and `acc`, closed over by the function that loops on them): the read is
    // an SSA value, so it names one array for the whole function, and nothing
    // in a buffer-safe function can store to the binding or run code that
    // would. Like a construction, the cursor follows the read itself.
    const bool is_binding_read = raw_obj->is_instruction() && fn_safe &&
                                 defined_by_binding_read(*fn_ast, obj_id);
    if (!is_fn_param && !is_construct && !is_binding_read) return false;

    BasicBlock* resume_bb = b.current_block();
    const DebugLoc saved_loc = b.current_loc();
    bool repositioned = false;
    if (is_fn_param) {
        BasicBlock* target_bb = lowering->entry_hoist_block();
        if (!target_bb) return false;
        if (target_bb != resume_bb) {
            if (!target_bb->tail()) return false;
            b.clear_current_loc();
            b.position_before(target_bb->tail());
            repositioned = true;
        }
    } else {
        Instruction* def_inst = raw_obj->defining_instruction();
        if (!def_inst || !def_inst->parent()) return false;
        if (def_inst->next() != nullptr) {
            b.clear_current_loc();
            b.position_after(def_inst);
            repositioned = true;
        }
    }

    Value* obj_i64 = lowering->ensure_type(raw_obj, Type::i64(), b);
    Value* ptr = b.build_and(obj_i64, b.build_iconst_i64(0x0000FFFFFFFFFFFFLL));
    out_data = lowering->prop_lowering().load_typed_array_data(b, ptr);
    out_len = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET));

    if (repositioned) {
        b.position_at_end(resume_bb);
        b.set_current_loc(saved_loc);
    }
    cursors[obj_id] = {out_data, out_len};
    return true;
}

constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kMaxInlineDispIndex = (uint64_t{INT32_MAX} - kBronzeHdrBytes) / 8;

Value* iconst64(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

} // namespace

void PropertyLoweringHelper::load_dense_array_cursor(Builder& b, Value* obj, Value*& out_block, Value*& out_head) {
    Value* ptr = b.build_and(obj, iconst64(b, kPayloadMask));
    Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst64(b, kPayloadMask));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    out_block = b.build_add(block, b.build_shl(head, iconst64(b, 3)));
    out_head = nullptr;
}

Value* PropertyLoweringHelper::dense_array_slot_addr(Builder& b, Value* block, Value* head, uint32_t index) {
    if (!head) {
        return b.build_add(block, iconst64(b, kBronzeHdrBytes + static_cast<uint64_t>(index) * 8));
    }
    Value* slot = index == 0 ? head : b.build_add(head, iconst64(b, index));
    return b.build_add(b.build_add(block, b.build_shl(slot, iconst64(b, 3))), iconst64(b, kBronzeHdrBytes));
}

Value* PropertyLoweringHelper::load_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index,
                                                            bool check_hole) {
    Value* elem = nullptr;
    if (!head) {
        if (index <= kMaxInlineDispIndex) {
            const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(index) * 8);
            elem = b.build_load(Type::i64(), block, disp);
        } else {
            elem = b.build_load_indexed(Type::i64(), block, iconst64(b, index), 8, static_cast<int32_t>(kBronzeHdrBytes));
        }
    } else if (index <= kMaxInlineDispIndex) {
        const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(index) * 8);
        elem = b.build_load_indexed(Type::i64(), block, head, 8, disp);
    } else {
        Value* slot = b.build_add(head, iconst64(b, index));
        elem = b.build_load_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes));
    }
    if (check_hole) {
        Value* is_hole = b.build_eq(elem, iconst64(b, BRONZE_ABI_HOLE_BITS));
        elem = b.build_select(is_hole, iconst64(b, kUndefinedTag), elem);
    }
    return elem;
}

void PropertyLoweringHelper::store_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index,
                                                           Value* val) {
    if (!head) {
        if (index <= kMaxInlineDispIndex) {
            const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(index) * 8);
            b.build_store(Type::i64(), block, disp, val);
        } else {
            b.build_store_indexed(Type::i64(), block, iconst64(b, index), 8, static_cast<int32_t>(kBronzeHdrBytes), val);
        }
    } else if (index <= kMaxInlineDispIndex) {
        const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(index) * 8);
        b.build_store_indexed(Type::i64(), block, head, 8, disp, val);
    } else {
        Value* slot = b.build_add(head, iconst64(b, index));
        b.build_store_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes), val);
    }
}

Value* PropertyLoweringHelper::lower_prop_get_index(Builder& b, Value* obj, uint32_t key_index,
                                                    uint32_t elem_index, const IcSite& site,
                                                    bool check_hole) {
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_dense_array_guard(b, obj, elem_index, /*writable=*/false, ok, block, head);
    BasicBlock* guard_bb = b.current_block();

    Function* fn = guard_bb->parent();
    const std::string prefix = "prop_idx_get_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(guard_bb);
    b.build_br_if(ok, bb_fast, bb_slow);

    b.position_at_end(bb_fast);
    Value* fast_val = load_proven_dense_array_elem(b, block, head, elem_index, check_hole);
    b.build_br(bb_merge, {fast_val});

    b.position_at_end(bb_slow);
    Value* slow_val = lower_prop_get(b, obj, key_index, site ? site_ptr(b, site) : nullptr);
    b.build_br(bb_merge, {slow_val});

    b.position_at_end(bb_merge);
    return result;
}

Value* PropertyLoweringHelper::lower_prop_get_array_length(Builder& b, Value* obj, uint32_t key_index,
                                                          Value* ic_entry) {
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_payload(b, obj, ptr, is_obj);
    BasicBlock* cur = b.current_block();
    const std::string prefix = "arr_len_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(cur);
    b.build_br_if(is_obj, bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
    Value* is_array = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow));
    b.build_br_if(is_array, bb_fast, bb_slow);

    b.position_at_end(bb_fast);
    Value* len = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    b.build_br(bb_merge, {box_number(b, b.build_sitofp_f64_i64(len))});

    b.position_at_end(bb_slow);
    b.build_br(bb_merge, {lower_prop_get(b, obj, key_index, ic_entry)});

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::lower_prop_set_index(Builder& b, Value* obj, uint32_t key_index,
                                                  uint32_t elem_index, Value* val, uint32_t strict,
                                                  const IcSite& site, bool val_is_primitive) {
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_dense_array_guard(b, obj, elem_index, /*writable=*/true, ok, block, head);
    BasicBlock* guard_bb = b.current_block();

    const bool need_barrier = !val_is_primitive && !is_known_non_ref(val);
    Function* fn = guard_bb->parent();
    const std::string prefix = "prop_idx_set_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_barrier = need_barrier ? b.append_block(prefix + "_barrier") : nullptr;
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(guard_bb);
    b.build_br_if(ok, bb_fast, bb_slow);

    b.position_at_end(bb_fast);
    store_proven_dense_array_elem(b, block, head, elem_index, val);
    if (need_barrier) {
        Value* is_ref = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_NUMBER_MAX_BITS)));
        b.build_br_if(is_ref, bb_barrier, bb_merge);
        b.position_at_end(bb_barrier);
        Value* slot_addr = dense_array_slot_addr(b, block, head, elem_index);
        b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, val});
        b.build_br(bb_merge);
    } else {
        b.build_br(bb_merge);
    }

    b.position_at_end(bb_slow);
    lower_prop_set(b, obj, key_index, val, strict, site ? site_ptr(b, site) : nullptr);
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
}

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
            if (!inst_ast.operands.empty() && prop_lowering.key_tag(inst_ast.index) == "length") {
                Value* ta_data = nullptr;
                Value* ta_len = nullptr;
                if (get_preloaded_ta_cursor(lowering, inst_ast.operands[0], b, val_map, ta_data, ta_len)) {
                    res_val = prop_lowering.box_number(b, b.build_sitofp_f64_i64(ta_len));
                    return true;
                }
            }
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            const IcSite site = prop_lowering.ic_site_ref(b, inst_ast.ic_index);
            if (inst_ast.index != PropertyLoweringHelper::kNoKey && inst_ast.static_slot == UINT32_MAX) {
                if (const auto elem_idx = bronze::il::parseIndexKey(prop_lowering.key_tag(inst_ast.index))) {
                    if (prop_lowering.enable_inlined_fastpaths() && prop_lowering.inline_sets()) {
                        const bool check_hole = !lowering->is_hole_insensitive(inst_ast.result_id);
                        if (lowering->is_proven_dense_array(inst_ast.operands[0], *elem_idx)) {
                            Value* block = nullptr;
                            Value* head = nullptr;
                            lowering->get_dense_array_cursor(inst_ast.operands[0], obj_val, b, block, head);
                            res_val = prop_lowering.load_proven_dense_array_elem(b, block, head, *elem_idx, check_hole);
                            return true;
                        }
                        if (!lowering->run_once()) {
                            res_val = prop_lowering.lower_prop_get_index(b, obj_val, inst_ast.index, *elem_idx, site,
                                                                         check_hole);
                            lowering->invalidate_dense_array_proofs();
                            return true;
                        }
                    }
                    res_val = prop_lowering.lower_prop_get(b, obj_val, inst_ast.index,
                                                           site ? prop_lowering.site_ptr(b, site) : nullptr);
                    lowering->invalidate_dense_array_proofs();
                    return true;
                }
            }
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
            // `x.length` where nothing proved what `x` is: an Array answers
            // from its header (10.4.2's exotic `length`, which the runtime's
            // read answers the same way for every array, `arguments`
            // included), and anything else takes the read it took before.
            // An array is never a plain object, so the mono guard below could
            // only ever miss on one.
            if (!mono && prop_lowering.enable_inlined_fastpaths() && !lowering->run_once() &&
                prop_lowering.key_tag(inst_ast.index) == "length") {
                res_val = prop_lowering.lower_prop_get_array_length(
                    b, obj_val, inst_ast.index, site ? prop_lowering.site_ptr(b, site) : nullptr);
                return true;
            }
            if (site && mono && prop_lowering.enable_inlined_fastpaths() &&
                inst_ast.index != PropertyLoweringHelper::kNoKey && !lowering->run_once()) {
                res_val = prop_lowering.lower_prop_get_mono(b, obj_val, inst_ast.index, site, inst_ast.is_family);
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
            if (inst_ast.index != PropertyLoweringHelper::kNoKey && inst_ast.static_slot == UINT32_MAX) {
                if (const auto elem_idx = bronze::il::parseIndexKey(prop_lowering.key_tag(inst_ast.index))) {
                    if (prop_lowering.enable_inlined_fastpaths() && prop_lowering.inline_sets() &&
                        !lowering->run_once()) {
                        const bool val_prim = lowering->is_primitive_value(inst_ast.operands[1]);
                        prop_lowering.lower_prop_set_index(b, obj_val, inst_ast.index, *elem_idx, val, strict, site,
                                                           val_prim);
                        lowering->invalidate_dense_array_proofs();
                        return true;
                    }
                    prop_lowering.lower_prop_set(b, obj_val, inst_ast.index, val, strict,
                                                 site ? prop_lowering.site_ptr(b, site) : nullptr);
                    lowering->invalidate_dense_array_proofs();
                    return true;
                }
            }
            // The same sites the read inlines (see PropGet) get the inline
            // own-slot store, less the writes to an object this function just
            // created. Every write in a class constructor (`this.x = x`) gets
            // it with the inline shape transition on its miss, which is what
            // answers those adds on every construction after the first. The
            // arm is code at every site it is put at: at every write, and at
            // the literals' adds too, three.js grew by a sixth and its scene
            // graph ran slower for the instruction cache it took.
            const bool mono = (inst_ast.is_mono || prop_lowering.feedback_driven()) &&
                              !lowering->is_fresh_object(inst_ast.operands[0]);
            const bool adds = lowering->in_class_constructor();
            if (site && (mono || adds) && prop_lowering.enable_inlined_fastpaths() &&
                prop_lowering.inline_sets() && inst_ast.index != PropertyLoweringHelper::kNoKey &&
                !lowering->run_once()) {
                const bool val_prim = lowering->is_primitive_value(inst_ast.operands[1]);
                prop_lowering.lower_prop_set_mono(b, obj_val, inst_ast.index, val, strict, site, val_prim,
                                                  /*with_transition=*/adds);
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

        case BronzeOp::ElemGet: {
            Value* obj_val = ensure_type(get_opd(0), Type::i64());
            Value* idx_val = get_opd(1);
            if (!idx_val) return false;
            res_val = lowering->run_once() ? prop_lowering.lower_elem_get_call(b, obj_val, idx_val)
                                           : prop_lowering.lower_elem_get(b, obj_val, idx_val);
            return true;
        }

        case BronzeOp::ElemGetTyped: {
            Value* idx_val = get_opd(1);
            if (!idx_val) return false;
            if (lowering->run_once()) {
                Value* obj_val = ensure_type(get_opd(0), Type::i64());
                res_val = prop_lowering.lower_elem_get_call(b, obj_val, idx_val);
                if (inst_ast.result_type == BronzeType::F64) {
                    res_val = ensure_type(res_val, Type::f64());
                }
            } else {
                Value* ta_data = nullptr;
                Value* ta_len = nullptr;
                if (!inst_ast.operands.empty()) {
                    get_preloaded_ta_cursor(lowering, inst_ast.operands[0], b, val_map, ta_data, ta_len);
                }
                const bool proven_in_bounds = (inst_ast.imm_i64 & (1LL << 17)) != 0;
                const bool need_obj = !ta_data || (!ta_len && !proven_in_bounds);
                Value* obj_val = need_obj ? ensure_type(get_opd(0), Type::i64()) : nullptr;
                res_val = prop_lowering.lower_elem_get_typed(b, obj_val, idx_val, inst_ast.imm_i64, ta_data, ta_len);
                if (inst_ast.result_type != BronzeType::F64) {
                    res_val = ensure_type(res_val, Type::i64());
                }
            }
            return true;
        }

        case BronzeOp::ElemSet: {
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

        case BronzeOp::ElemSetTyped: {
            Value* idx_val = get_opd(1);
            if (!idx_val) return false;
            Value* raw_val = get_opd(2);
            if (!raw_val) return false;
            if (lowering->run_once()) {
                Value* obj_val = ensure_type(get_opd(0), Type::i64());
                prop_lowering.lower_elem_set_call(b, obj_val, idx_val, ensure_type(raw_val, Type::i64()),
                                                  inst_ast.index);
            } else {
                Value* ta_data = nullptr;
                Value* ta_len = nullptr;
                if (!inst_ast.operands.empty()) {
                    get_preloaded_ta_cursor(lowering, inst_ast.operands[0], b, val_map, ta_data, ta_len);
                }
                const bool proven_in_bounds = (inst_ast.imm_i64 & (1LL << 17)) != 0;
                const bool need_obj = !ta_data || (!ta_len && !proven_in_bounds);
                Value* obj_val = need_obj ? ensure_type(get_opd(0), Type::i64()) : nullptr;
                prop_lowering.lower_elem_set_typed(b, obj_val, idx_val, ensure_type(raw_val, Type::f64()),
                                                   inst_ast.imm_i64, inst_ast.index, ta_data, ta_len);
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

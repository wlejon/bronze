// Batched lowering of contiguous variable-offset element read/write runs:
//   - `toArray(array, offset)`: `array[offset + k] = te[k]` or `array[offset + k] = this.<prop_k>`
//   - `fromArray(array, offset)`: `this.<prop_k> = array[offset + k]` or `te[k] = array[offset + k]`
//
// Fuses all receiver/index/bounds/shape checks into a single guard and lowers the
// fast path into straight-line indexed memory operations for Float32Array,
// Float64Array, and dense Array targets/sources, with a single helper-only slow
// block for everything else.

#include "il_lowering.h"
#include "il/key.h"
#include "il_abi.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace il2mir {

namespace {

constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kTypedArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr uint64_t kMaxSafeBaseIndex = 0x7FFFFFFFULL;

Value* iconst(Builder& b, uint64_t v) {
    return b.build_iconst_i64(static_cast<int64_t>(v));
}

bool is_const_f64_int(const BronzeInstruction& inst, uint32_t expected) {
    return inst.op == BronzeOp::ConstF64 && inst.result_id != UINT32_MAX &&
           inst.imm_f64 == static_cast<double>(expected) && !std::signbit(inst.imm_f64);
}

enum class ElemRunPeerKind : uint8_t {
    None,
    DenseArray,
    MonoObj,
    Precomputed,
};

struct ElemSetStep {
    size_t next_pos = 0;
    uint32_t base_id = UINT32_MAX;
    uint32_t arr_id = UINT32_MAX;
    uint32_t strict = 0;
    ElemRunPeerKind src_kind = ElemRunPeerKind::None;
    uint32_t src_obj_id = UINT32_MAX;
    uint32_t src_elem_idx = 0;
    uint32_t src_key_idx = PropertyLoweringHelper::kNoKey;
    uint32_t src_ic_idx = UINT32_MAX;
    uint32_t precomputed_val_id = UINT32_MAX;
};

struct ElemGetStep {
    size_t next_pos = 0;
    uint32_t base_id = UINT32_MAX;
    uint32_t arr_id = UINT32_MAX;
    ElemRunPeerKind dst_kind = ElemRunPeerKind::None;
    uint32_t dst_obj_id = UINT32_MAX;
    uint32_t dst_elem_idx = 0;
    uint32_t dst_key_idx = PropertyLoweringHelper::kNoKey;
    uint32_t dst_ic_idx = UINT32_MAX;
};

// Matches the index expression for step `k` starting at `pos`:
//   - k == 0: either `%idx = box.f64 %base` at `pos`, or no index instructions
//     (`idx_id` is matched directly from the `elem.set`/`elem.get` operand).
//   - k >= 1, shape A (f64 base):
//       `%c = const.f64 k; %sum = add.f64 %base, %c; %idx = box.f64 %sum`
//   - k >= 1, shape B (dynamic base):
//       `%c = const.f64 k; %bc = box.f64 %c; %idx = add.dyn %base, %bc`
bool match_step_index(const std::vector<BronzeInstruction>& insts, size_t pos, uint32_t k,
                      uint32_t expected_base_id,
                      const std::unordered_map<uint32_t, uint32_t>& use_counts,
                      size_t& out_after_idx, uint32_t& out_base_id, uint32_t& out_idx_id) {
    auto single_use = [&](uint32_t id) -> bool {
        if (id == UINT32_MAX) return false;
        const auto it = use_counts.find(id);
        return it != use_counts.end() && it->second == 1;
    };

    if (k == 0) {
        if (pos < insts.size()) {
            const BronzeInstruction& i0 = insts[pos];
            if (i0.op == BronzeOp::Box && i0.box_type == BronzeType::F64 &&
                i0.operands.size() == 1 && single_use(i0.result_id)) {
                out_base_id = i0.operands[0];
                out_idx_id = i0.result_id;
                out_after_idx = pos + 1;
                return true;
            }
        }
        out_base_id = UINT32_MAX;
        out_idx_id = UINT32_MAX;
        out_after_idx = pos;
        return true;
    }

    if (pos + 3 > insts.size()) return false;
    const BronzeInstruction& i0 = insts[pos];
    const BronzeInstruction& i1 = insts[pos + 1];
    const BronzeInstruction& i2 = insts[pos + 2];
    if (!is_const_f64_int(i0, k) || !single_use(i0.result_id)) return false;
    const uint32_t c_id = i0.result_id;

    // Shape A: add.f64 then box.f64
    if (i1.op == BronzeOp::Add && i1.result_type == BronzeType::F64 && i1.operands.size() == 2 &&
        single_use(i1.result_id) &&
        ((i1.operands[0] == expected_base_id && i1.operands[1] == c_id) ||
         (i1.operands[1] == expected_base_id && i1.operands[0] == c_id)) &&
        i2.op == BronzeOp::Box && i2.box_type == BronzeType::F64 && i2.operands.size() == 1 &&
        i2.operands[0] == i1.result_id && single_use(i2.result_id)) {
        out_base_id = expected_base_id;
        out_idx_id = i2.result_id;
        out_after_idx = pos + 3;
        return true;
    }

    // Shape B: box.f64 then add.dynamic
    if (i1.op == BronzeOp::Box && i1.box_type == BronzeType::F64 && i1.operands.size() == 1 &&
        i1.operands[0] == c_id && single_use(i1.result_id) &&
        i2.op == BronzeOp::Add &&
        (i2.result_type == BronzeType::Dynamic || i2.result_type == BronzeType::Unknown) &&
        i2.operands.size() == 2 && single_use(i2.result_id) &&
        ((i2.operands[0] == expected_base_id && i2.operands[1] == i1.result_id) ||
         (i2.operands[1] == expected_base_id && i2.operands[0] == i1.result_id))) {
        out_base_id = expected_base_id;
        out_idx_id = i2.result_id;
        out_after_idx = pos + 3;
        return true;
    }

    return false;
}

} // namespace

bool IlLowering::try_lower_elem_offset_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                           std::unordered_map<uint32_t, Value*>& val_map,
                                           const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                           uint32_t* cont_counter) {
    if (run_once_ || !prop_lowering_.enable_inlined_fastpaths() || !prop_lowering_.inline_sets()) {
        return false;
    }

    const auto& insts = blk_ast.instructions;
    auto single_use = [&](uint32_t id) -> bool {
        if (id == UINT32_MAX) return false;
        const auto it = value_use_counts_.find(id);
        return it != value_use_counts_.end() && it->second == 1;
    };

    auto apply_loc = [&](const BronzeInstruction& inst) { apply_inst_loc(b, inst); };

    // --- 1. Try matching a contiguous ElemSet run (`toArray`) ---
    auto classify_prop_peer = [&](const BronzeInstruction& prop_inst, ElemRunPeerKind& out_kind,
                                  uint32_t& out_elem_idx) -> bool {
        if (prop_inst.index == PropertyLoweringHelper::kNoKey) return false;
        if (prop_inst.static_slot == UINT32_MAX) {
            if (const auto parsed = bronze::il::parseIndexKey(prop_lowering_.key_tag(prop_inst.index))) {
                out_kind = ElemRunPeerKind::DenseArray;
                out_elem_idx = *parsed;
                return true;
            }
        }
        if ((prop_inst.is_mono || prop_inst.static_slot != UINT32_MAX) &&
            prop_inst.ic_index < prop_lowering_.ic_site_count()) {
            out_kind = ElemRunPeerKind::MonoObj;
            out_elem_idx = 0;
            return true;
        }
        return false;
    };

    auto match_elem_set_step = [&](size_t pos, uint32_t k, uint32_t expected_base, uint32_t expected_arr,
                                   ElemRunPeerKind expected_src_kind, uint32_t expected_src_obj,
                                   ElemSetStep& out) -> bool {
        // Pattern 1A ([fast 0] in toArray):
        //   k == 0: `%v = prop.get %src, key; %idx = box.f64 %base; elem.set %arr, %idx, %v`
        //   k >= 1: `%c = const.f64 k; %sum = add.f64 %base, %c; %v = prop.get %src, key; %idx = box.f64 %sum; elem.set %arr, %idx, %v`
        if (k == 0 && pos + 3 <= insts.size() && insts[pos].op == BronzeOp::PropGet &&
            insts[pos + 1].op == BronzeOp::Box && insts[pos + 1].box_type == BronzeType::F64 &&
            insts[pos + 2].op == BronzeOp::ElemSet) {
            const BronzeInstruction& pg = insts[pos];
            const BronzeInstruction& bx = insts[pos + 1];
            const BronzeInstruction& es = insts[pos + 2];
            ElemRunPeerKind src_kind = ElemRunPeerKind::None;
            uint32_t src_elem_idx = 0;
            if (pg.operands.size() == 1 && single_use(pg.result_id) &&
                bx.operands.size() == 1 && single_use(bx.result_id) &&
                es.operands.size() >= 3 && es.operands[1] == bx.result_id && es.operands[2] == pg.result_id &&
                classify_prop_peer(pg, src_kind, src_elem_idx)) {
                const uint32_t base_id = bx.operands[0];
                const uint32_t arr_id = es.operands[0];
                const uint32_t src_obj = pg.operands[0];
                if (src_obj != arr_id && base_id != pg.result_id) {
                    out.next_pos = pos + 3;
                    out.base_id = base_id;
                    out.arr_id = arr_id;
                    out.strict = es.index;
                    out.src_kind = src_kind;
                    out.src_obj_id = src_obj;
                    out.src_elem_idx = src_elem_idx;
                    out.src_key_idx = pg.index;
                    out.src_ic_idx = pg.ic_index;
                    return true;
                }
            }
        } else if (k >= 1 && pos + 5 <= insts.size() && is_const_f64_int(insts[pos], k) &&
                   single_use(insts[pos].result_id) && insts[pos + 1].op == BronzeOp::Add &&
                   insts[pos + 1].result_type == BronzeType::F64 && insts[pos + 1].operands.size() == 2 &&
                   single_use(insts[pos + 1].result_id) && insts[pos + 2].op == BronzeOp::PropGet &&
                   insts[pos + 3].op == BronzeOp::Box && insts[pos + 3].box_type == BronzeType::F64 &&
                   insts[pos + 4].op == BronzeOp::ElemSet) {
            const uint32_t c_id = insts[pos].result_id;
            const BronzeInstruction& add_inst = insts[pos + 1];
            const BronzeInstruction& pg = insts[pos + 2];
            const BronzeInstruction& bx = insts[pos + 3];
            const BronzeInstruction& es = insts[pos + 4];
            ElemRunPeerKind src_kind = ElemRunPeerKind::None;
            uint32_t src_elem_idx = 0;
            if (((add_inst.operands[0] == expected_base && add_inst.operands[1] == c_id) ||
                 (add_inst.operands[1] == expected_base && add_inst.operands[0] == c_id)) &&
                pg.operands.size() == 1 && single_use(pg.result_id) &&
                bx.operands.size() == 1 && bx.operands[0] == add_inst.result_id && single_use(bx.result_id) &&
                es.operands.size() >= 3 && es.operands[0] == expected_arr && es.operands[1] == bx.result_id &&
                es.operands[2] == pg.result_id && pg.operands[0] == expected_src_obj &&
                classify_prop_peer(pg, src_kind, src_elem_idx) && src_kind == expected_src_kind) {
                out.next_pos = pos + 5;
                out.base_id = expected_base;
                out.arr_id = expected_arr;
                out.strict = es.index;
                out.src_kind = src_kind;
                out.src_obj_id = expected_src_obj;
                out.src_elem_idx = src_elem_idx;
                out.src_key_idx = pg.index;
                out.src_ic_idx = pg.ic_index;
                return true;
            }
        }

        // Pattern 1B: index chain followed by PropGet + ElemSet (or ElemSet of precomputed value)
        size_t after_idx = pos;
        uint32_t base_id = UINT32_MAX;
        uint32_t idx_id = UINT32_MAX;
        if (!match_step_index(insts, pos, k, expected_base, value_use_counts_, after_idx, base_id, idx_id)) {
            return false;
        }
        if (after_idx >= insts.size()) return false;

        // Option 1: PropGet followed by ElemSet
        if (after_idx + 1 < insts.size() && insts[after_idx].op == BronzeOp::PropGet &&
            insts[after_idx + 1].op == BronzeOp::ElemSet) {
            const BronzeInstruction& pg = insts[after_idx];
            const BronzeInstruction& es = insts[after_idx + 1];
            if (pg.operands.size() != 1 || !single_use(pg.result_id)) {
                return false;
            }
            if (es.operands.size() < 3 || es.operands[2] != pg.result_id) return false;
            const uint32_t arr_id = es.operands[0];
            const uint32_t es_idx_id = es.operands[1];
            if (idx_id == UINT32_MAX) {
                base_id = es_idx_id;
                idx_id = es_idx_id;
            } else if (es_idx_id != idx_id) {
                return false;
            }
            const uint32_t src_obj = pg.operands[0];
            if (src_obj == arr_id || base_id == pg.result_id) return false;
            if (k > 0 && (arr_id != expected_arr || base_id != expected_base || src_obj != expected_src_obj)) {
                return false;
            }

            ElemRunPeerKind src_kind = ElemRunPeerKind::None;
            uint32_t src_elem_idx = 0;
            if (!classify_prop_peer(pg, src_kind, src_elem_idx)) return false;
            if (k > 0 && src_kind != expected_src_kind) return false;

            out.next_pos = after_idx + 2;
            out.base_id = base_id;
            out.arr_id = arr_id;
            out.strict = es.index;
            out.src_kind = src_kind;
            out.src_obj_id = src_obj;
            out.src_elem_idx = src_elem_idx;
            out.src_key_idx = pg.index;
            out.src_ic_idx = pg.ic_index;
            return true;
        }

        // Option 2: ElemSet of a precomputed value defined before the run
        if (insts[after_idx].op == BronzeOp::ElemSet) {
            const BronzeInstruction& es = insts[after_idx];
            if (es.operands.size() < 3) return false;
            const uint32_t arr_id = es.operands[0];
            const uint32_t es_idx_id = es.operands[1];
            const uint32_t val_id = es.operands[2];
            if (idx_id == UINT32_MAX) {
                base_id = es_idx_id;
                idx_id = es_idx_id;
            } else if (es_idx_id != idx_id) {
                return false;
            }
            if (val_id == idx_id || val_map.find(val_id) == val_map.end()) return false;
            if (k > 0 && (arr_id != expected_arr || base_id != expected_base ||
                          expected_src_kind != ElemRunPeerKind::Precomputed)) {
                return false;
            }

            out.next_pos = after_idx + 1;
            out.base_id = base_id;
            out.arr_id = arr_id;
            out.strict = es.index;
            out.src_kind = ElemRunPeerKind::Precomputed;
            out.precomputed_val_id = val_id;
            return true;
        }

        return false;
    };

    std::vector<ElemSetStep> set_steps;
    {
        size_t pos = inst_idx;
        uint32_t base_id = UINT32_MAX;
        uint32_t arr_id = UINT32_MAX;
        ElemRunPeerKind src_kind = ElemRunPeerKind::None;
        uint32_t src_obj = UINT32_MAX;
        for (uint32_t k = 0; k < 64; ++k) {
            ElemSetStep step;
            if (!match_elem_set_step(pos, k, base_id, arr_id, src_kind, src_obj, step)) break;
            if (k == 0) {
                if (val_map.find(step.base_id) == val_map.end() || val_map.find(step.arr_id) == val_map.end()) {
                    break;
                }
                if (step.src_obj_id != UINT32_MAX && val_map.find(step.src_obj_id) == val_map.end()) {
                    break;
                }
                base_id = step.base_id;
                arr_id = step.arr_id;
                src_kind = step.src_kind;
                src_obj = step.src_obj_id;
            }
            set_steps.push_back(step);
            pos = step.next_pos;
        }
    }

    if (set_steps.size() >= 3) {
        const size_t N = set_steps.size();
        const size_t committed_end = set_steps.back().next_pos;
        const uint32_t arr_id = set_steps[0].arr_id;
        const uint32_t base_id = set_steps[0].base_id;
        const ElemRunPeerKind src_kind = set_steps[0].src_kind;
        const uint32_t src_obj_id = set_steps[0].src_obj_id;

        apply_loc(insts[inst_idx]);
        Value* arr_obj = ensure_type(get_val_by_id(arr_id, b, val_map), Type::i64(), b);
        Value* arr_ptr = nullptr;
        Value* arr_is_obj = nullptr;
        prop_lowering_.object_payload(b, arr_obj, arr_ptr, arr_is_obj);

        Value* base_val = get_val_by_id(base_id, b, val_map);
        Value* base_i64 = nullptr;
        Value* idx_ok = nullptr;
        prop_lowering_.emit_elem_index_guard(b, base_val, base_i64, idx_ok);
        Value* base_nonneg = b.build_ult(base_i64, iconst(b, kMaxSafeBaseIndex));
        Value* guard_ok = b.build_and(arr_is_obj, b.build_and(idx_ok, base_nonneg));

        Value* src_block = nullptr;
        Value* src_head = nullptr;
        Value* src_ptr = nullptr;
        std::vector<Value*> src_slot_words(N, nullptr);

        if (src_kind == ElemRunPeerKind::DenseArray) {
            uint32_t max_src_idx = 0;
            for (const auto& st : set_steps) max_src_idx = std::max(max_src_idx, st.src_elem_idx);
            Value* src_obj = ensure_type(get_val_by_id(src_obj_id, b, val_map), Type::i64(), b);
            if (is_proven_dense_array(src_obj_id, max_src_idx)) {
                get_dense_array_cursor(src_obj_id, src_obj, b, src_block, src_head);
            } else {
                Value* src_ok = nullptr;
                prop_lowering_.emit_dense_array_guard(b, src_obj, max_src_idx, /*writable=*/false,
                                                      src_ok, src_block, src_head);
                guard_ok = b.build_and(guard_ok, src_ok);
            }
        } else if (src_kind == ElemRunPeerKind::MonoObj) {
            Value* src_obj = ensure_type(get_val_by_id(src_obj_id, b, val_map), Type::i64(), b);
            Value* src_plain = nullptr;
            Value* src_shape = nullptr;
            prop_lowering_.receiver_shape(b, src_obj, src_plain, src_ptr, src_shape);
            for (size_t k = 0; k < N; ++k) {
                const IcSite site = prop_lowering_.ic_site_ref(b, set_steps[k].src_ic_idx);
                Value* hit = nullptr;
                prop_lowering_.emit_mono_slot_guard(b, src_shape, src_plain, site, hit, src_slot_words[k]);
                guard_ok = b.build_and(guard_ok, hit);
            }
        }

        BasicBlock* guard_bb = b.current_block();
        const std::string prefix = "elem_set_run_" + std::to_string(fn->next_block_id());
        BasicBlock* bb_src_load = b.append_block(prefix + "_src");
        BasicBlock* bb_ta_check = b.append_block(prefix + "_ta_check");
        BasicBlock* bb_ta = b.append_block(prefix + "_ta");
        BasicBlock* bb_ta_f32 = b.append_block(prefix + "_ta_f32");
        BasicBlock* bb_ta_f64 = b.append_block(prefix + "_ta_f64");
        BasicBlock* bb_arr = b.append_block(prefix + "_arr");
        BasicBlock* bb_arr_store = b.append_block(prefix + "_arr_store");
        BasicBlock* bb_slow = b.append_block(prefix + "_slow");
        BasicBlock* bb_merge = b.append_block(prefix + "_merge");

        b.position_at_end(guard_bb);
        b.build_br_if(guard_ok, bb_src_load, bb_slow);

        b.position_at_end(bb_src_load);
        std::vector<Value*> vals_f64(N, nullptr);
        std::vector<Value*> vals_boxed(N, nullptr);
        Value* all_num_ok = nullptr;
        // A dense-array source is only tested here; each store block below loads its element
        // again, right before the store. Holding all N elements, boxed and unboxed, across the
        // target's header checks kept 2N values live and spilled most of them (toArray's 16).
        // The second load sees the bits the test saw: between them run only loads of the
        // target's header and branches, which neither run user code nor collect, so the
        // elements block cannot move or change.
        const bool reload_src = src_kind == ElemRunPeerKind::DenseArray;
        auto src_elem = [&](size_t k) -> Value* {
            // `check_hole = false`: BRONZE_ABI_HOLE_BITS > kBronzeNumberMaxBits, so a hole
            // is rejected by the number test without a separate select per load.
            return prop_lowering_.load_proven_dense_array_elem(b, src_block, src_head, set_steps[k].src_elem_idx,
                                                               /*check_hole=*/false);
        };
        for (size_t k = 0; k < N; ++k) {
            Value* raw_val = nullptr;
            if (reload_src) {
                raw_val = src_elem(k);
            } else if (src_kind == ElemRunPeerKind::MonoObj) {
                raw_val = prop_lowering_.load_slot(b, src_ptr, src_slot_words[k]);
            } else {
                raw_val = get_val_by_id(set_steps[k].precomputed_val_id, b, val_map);
            }
            Value* num_ok = nullptr;
            Value* as_f64 = prop_lowering_.unbox_known_number(b, raw_val, num_ok);
            if (!reload_src) {
                vals_f64[k] = as_f64;
                vals_boxed[k] = (raw_val->type() == Type::i64()) ? raw_val : prop_lowering_.box_number(b, as_f64);
            }
            if (num_ok) {
                all_num_ok = all_num_ok ? b.build_and(all_num_ok, num_ok) : num_ok;
            }
        }
        // The element bits for store `k`, in the store's own block (see `reload_src`).
        auto elem_bits = [&](size_t k) -> Value* { return reload_src ? src_elem(k) : vals_boxed[k]; };
        auto elem_f64 = [&](size_t k) -> Value* {
            return reload_src ? b.build_bitcast_f64_i64(src_elem(k)) : vals_f64[k];
        };
        Value* max_end_idx = b.build_add(base_i64, iconst(b, N - 1));
        Value* header_low = b.build_load(Type::i32(), arr_ptr, 0);
        Value* is_arr = b.build_eq(header_low, b.build_iconst_i32(kArrayHeaderLow));
        if (all_num_ok) {
            b.build_br_if(all_num_ok, bb_ta_check, bb_slow);
            b.position_at_end(bb_ta_check);
        } else {
            b.build_br(bb_ta_check);
            b.position_at_end(bb_ta_check);
        }
        b.build_br_if(is_arr, bb_arr, bb_ta);

        b.position_at_end(bb_ta);
        Value* is_ta = b.build_eq(header_low, b.build_iconst_i32(kTypedArrayHeaderLow));
        Value* ta_len = b.build_zext_i64(b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
        Value* ta_in_bounds = b.build_and(is_ta, b.build_ult(max_end_idx, ta_len));
        Value* ta_kind = b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_TA_KIND_OFFSET);
        Value* is_f32 = b.build_and(ta_in_bounds, b.build_eq(ta_kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT32)));
        BasicBlock* bb_ta_f64_check = b.append_block(prefix + "_ta_f64_check");
        b.position_at_end(bb_ta);
        b.build_br_if(is_f32, bb_ta_f32, bb_ta_f64_check);

        b.position_at_end(bb_ta_f64_check);
        Value* is_f64 = b.build_and(ta_in_bounds, b.build_eq(ta_kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT64)));
        b.build_br_if(is_f64, bb_ta_f64, bb_slow);

        b.position_at_end(bb_ta_f32);
        Value* f32_data = prop_lowering_.load_typed_array_data(b, arr_ptr);
        for (size_t k = 0; k < N; ++k) {
            b.build_store_indexed(Type::f32(), f32_data, base_i64, 4, static_cast<int32_t>(k * 4),
                                  b.build_fptrunc_f32_f64(elem_f64(k)));
        }
        b.build_br(bb_merge);

        b.position_at_end(bb_ta_f64);
        Value* f64_data = prop_lowering_.load_typed_array_data(b, arr_ptr);
        for (size_t k = 0; k < N; ++k) {
            b.build_store_indexed(Type::f64(), f64_data, base_i64, 8, static_cast<int32_t>(k * 8), elem_f64(k));
        }
        b.build_br(bb_merge);

        b.position_at_end(bb_arr);
        Value* arr_len = b.build_zext_i64(b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
        Value* arr_cap = b.build_zext_i64(b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
        Value* arr_head = b.build_zext_i64(b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
        Value* base_slot = b.build_add(base_i64, arr_head);
        Value* max_slot = b.build_add(max_end_idx, arr_head);
        Value* props = b.build_load(Type::i64(), arr_ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
        Value* no_props = b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
        Value* arr_in_bounds = b.build_and(b.build_ult(max_end_idx, arr_len), b.build_ult(max_slot, arr_cap));
        b.build_br_if(b.build_and(arr_in_bounds, no_props), bb_arr_store, bb_slow);

        b.position_at_end(bb_arr_store);
        Value* arr_block =
            b.build_and(b.build_load(Type::i64(), arr_ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
        for (size_t k = 0; k < N; ++k) {
            const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(k) * 8);
            b.build_store_indexed(Type::i64(), arr_block, base_slot, 8, disp, elem_bits(k));
        }
        // Every element stored is proven <= kBronzeNumberMaxBits, so no write barrier is needed.
        b.build_br(bb_merge);

        // Slow path: lower the slice in helper-only mode (`run_once_ = true`).
        b.position_at_end(bb_slow);
        invalidate_dense_array_proofs();
        const bool saved_run_once = run_once_;
        run_once_ = true;
        for (size_t j = inst_idx; j < committed_end; ++j) {
            apply_loc(insts[j]);
            if (!lower_instruction(insts[j], b, fn, val_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
                run_once_ = saved_run_once;
                return false;
            }
        }
        run_once_ = saved_run_once;
        b.build_br(bb_merge);

        b.position_at_end(bb_merge);
        invalidate_dense_array_proofs();
        inst_idx = committed_end - 1;
        return true;
    }

    // --- 2. Try matching a contiguous ElemGet run (`fromArray`) ---
    auto match_elem_get_step = [&](size_t pos, uint32_t k, uint32_t expected_base, uint32_t expected_arr,
                                   ElemRunPeerKind expected_dst_kind, uint32_t expected_dst_obj,
                                   ElemGetStep& out) -> bool {
        size_t after_idx = pos;
        uint32_t base_id = UINT32_MAX;
        uint32_t idx_id = UINT32_MAX;
        if (!match_step_index(insts, pos, k, expected_base, value_use_counts_, after_idx, base_id, idx_id)) {
            return false;
        }
        if (after_idx + 1 >= insts.size()) return false;
        const BronzeInstruction& eg = insts[after_idx];
        const BronzeInstruction& ps = insts[after_idx + 1];
        if (eg.op != BronzeOp::ElemGet || eg.operands.size() < 2 || !single_use(eg.result_id)) {
            return false;
        }
        if (ps.op != BronzeOp::PropSet || ps.operands.size() < 2 || ps.operands[1] != eg.result_id) {
            return false;
        }
        const uint32_t arr_id = eg.operands[0];
        const uint32_t eg_idx_id = eg.operands[1];
        if (idx_id == UINT32_MAX) {
            base_id = eg_idx_id;
            idx_id = eg_idx_id;
        } else if (eg_idx_id != idx_id) {
            return false;
        }
        const uint32_t dst_obj = ps.operands[0];
        if (dst_obj == arr_id || is_fresh_object(dst_obj)) return false;
        if (k > 0 && (arr_id != expected_arr || base_id != expected_base || dst_obj != expected_dst_obj)) {
            return false;
        }

        ElemRunPeerKind dst_kind = ElemRunPeerKind::None;
        uint32_t dst_elem_idx = 0;
        if (!classify_prop_peer(ps, dst_kind, dst_elem_idx)) return false;
        if (k > 0 && dst_kind != expected_dst_kind) return false;

        out.next_pos = after_idx + 2;
        out.base_id = base_id;
        out.arr_id = arr_id;
        out.dst_kind = dst_kind;
        out.dst_obj_id = dst_obj;
        out.dst_elem_idx = dst_elem_idx;
        out.dst_key_idx = ps.index;
        out.dst_ic_idx = ps.ic_index;
        return true;
    };

    std::vector<ElemGetStep> get_steps;
    {
        size_t pos = inst_idx;
        uint32_t base_id = UINT32_MAX;
        uint32_t arr_id = UINT32_MAX;
        ElemRunPeerKind dst_kind = ElemRunPeerKind::None;
        uint32_t dst_obj = UINT32_MAX;
        for (uint32_t k = 0; k < 64; ++k) {
            ElemGetStep step;
            if (!match_elem_get_step(pos, k, base_id, arr_id, dst_kind, dst_obj, step)) break;
            if (k == 0) {
                if (val_map.find(step.base_id) == val_map.end() || val_map.find(step.arr_id) == val_map.end() ||
                    val_map.find(step.dst_obj_id) == val_map.end()) {
                    break;
                }
                base_id = step.base_id;
                arr_id = step.arr_id;
                dst_kind = step.dst_kind;
                dst_obj = step.dst_obj_id;
            }
            get_steps.push_back(step);
            pos = step.next_pos;
        }
    }

    if (get_steps.size() >= 3) {
        const size_t N = get_steps.size();
        const size_t committed_end = get_steps.back().next_pos;
        const uint32_t arr_id = get_steps[0].arr_id;
        const uint32_t base_id = get_steps[0].base_id;
        const ElemRunPeerKind dst_kind = get_steps[0].dst_kind;
        const uint32_t dst_obj_id = get_steps[0].dst_obj_id;

        apply_loc(insts[inst_idx]);
        Value* arr_obj = ensure_type(get_val_by_id(arr_id, b, val_map), Type::i64(), b);
        Value* arr_ptr = nullptr;
        Value* arr_is_obj = nullptr;
        prop_lowering_.object_payload(b, arr_obj, arr_ptr, arr_is_obj);

        Value* base_val = get_val_by_id(base_id, b, val_map);
        Value* base_i64 = nullptr;
        Value* idx_ok = nullptr;
        prop_lowering_.emit_elem_index_guard(b, base_val, base_i64, idx_ok);
        Value* base_nonneg = b.build_ult(base_i64, iconst(b, kMaxSafeBaseIndex));
        Value* guard_ok = b.build_and(arr_is_obj, b.build_and(idx_ok, base_nonneg));

        Value* dst_block = nullptr;
        Value* dst_head = nullptr;
        Value* dst_ptr = nullptr;
        std::vector<Value*> dst_slot_words(N, nullptr);

        Value* dst_obj = ensure_type(get_val_by_id(dst_obj_id, b, val_map), Type::i64(), b);
        if (dst_kind == ElemRunPeerKind::DenseArray) {
            uint32_t max_dst_idx = 0;
            for (const auto& st : get_steps) max_dst_idx = std::max(max_dst_idx, st.dst_elem_idx);
            Value* dst_ok = nullptr;
            prop_lowering_.emit_dense_array_guard(b, dst_obj, max_dst_idx, /*writable=*/true,
                                                  dst_ok, dst_block, dst_head);
            guard_ok = b.build_and(guard_ok, dst_ok);
        } else {
            Value* dst_plain = nullptr;
            Value* dst_shape = nullptr;
            prop_lowering_.receiver_shape(b, dst_obj, dst_plain, dst_ptr, dst_shape);
            for (size_t k = 0; k < N; ++k) {
                const IcSite site = prop_lowering_.ic_site_ref(b, get_steps[k].dst_ic_idx);
                Value* hit = nullptr;
                prop_lowering_.emit_mono_slot_guard(b, dst_shape, dst_plain, site, hit, dst_slot_words[k]);
                guard_ok = b.build_and(guard_ok, hit);
            }
        }

        BasicBlock* guard_bb = b.current_block();
        const std::string prefix = "elem_get_run_" + std::to_string(fn->next_block_id());
        BasicBlock* bb_dispatch = b.append_block(prefix + "_dispatch");
        BasicBlock* bb_src_arr = b.append_block(prefix + "_src_arr");
        BasicBlock* bb_src_arr_load = b.append_block(prefix + "_src_arr_load");
        BasicBlock* bb_src_ta = b.append_block(prefix + "_src_ta");
        BasicBlock* bb_src_ta_f32 = b.append_block(prefix + "_src_ta_f32");
        BasicBlock* bb_src_ta_f64 = b.append_block(prefix + "_src_ta_f64");
        BasicBlock* bb_store = b.append_block(prefix + "_store");
        BasicBlock* bb_slow = b.append_block(prefix + "_slow");
        BasicBlock* bb_merge = b.append_block(prefix + "_merge");

        std::vector<Value*> store_params(N, nullptr);
        for (size_t k = 0; k < N; ++k) {
            store_params[k] = b.add_block_param(bb_store, Type::i64());
        }

        b.position_at_end(guard_bb);
        b.build_br_if(guard_ok, bb_dispatch, bb_slow);

        b.position_at_end(bb_dispatch);
        Value* max_end_idx = b.build_add(base_i64, iconst(b, N - 1));
        Value* header_low = b.build_load(Type::i32(), arr_ptr, 0);
        b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kArrayHeaderLow)), bb_src_arr, bb_src_ta);

        b.position_at_end(bb_src_arr);
        Value* arr_len = b.build_zext_i64(b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
        b.build_br_if(b.build_ult(max_end_idx, arr_len), bb_src_arr_load, bb_slow);

        b.position_at_end(bb_src_arr_load);
        Value* src_block =
            b.build_and(b.build_load(Type::i64(), arr_ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
        Value* src_head = b.build_zext_i64(b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
        Value* src_base_slot = b.build_add(base_i64, src_head);
        std::vector<Value*> arr_elems(N, nullptr);
        Value* all_num = nullptr;
        for (size_t k = 0; k < N; ++k) {
            const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(k) * 8);
            arr_elems[k] = b.build_load_indexed(Type::i64(), src_block, src_base_slot, 8, disp);
            Value* is_num = b.build_ule(arr_elems[k], iconst(b, kBronzeNumberMaxBits));
            all_num = all_num ? b.build_and(all_num, is_num) : is_num;
        }
        b.build_br_if(all_num, bb_store, arr_elems, bb_slow, {});

        b.position_at_end(bb_src_ta);
        Value* is_ta = b.build_eq(header_low, b.build_iconst_i32(kTypedArrayHeaderLow));
        Value* ta_len = b.build_zext_i64(b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
        Value* ta_in_bounds = b.build_and(is_ta, b.build_ult(max_end_idx, ta_len));
        Value* ta_kind = b.build_load(Type::i32(), arr_ptr, BRONZE_ABI_TA_KIND_OFFSET);
        Value* is_f32 = b.build_and(ta_in_bounds, b.build_eq(ta_kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT32)));
        BasicBlock* bb_src_ta_f64_check = b.append_block(prefix + "_src_ta_f64_check");
        b.position_at_end(bb_src_ta);
        b.build_br_if(is_f32, bb_src_ta_f32, bb_src_ta_f64_check);

        b.position_at_end(bb_src_ta_f64_check);
        Value* is_f64 = b.build_and(ta_in_bounds, b.build_eq(ta_kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT64)));
        b.build_br_if(is_f64, bb_src_ta_f64, bb_slow);

        b.position_at_end(bb_src_ta_f32);
        Value* f32_data = prop_lowering_.load_typed_array_data(b, arr_ptr);
        std::vector<Value*> f32_boxed(N, nullptr);
        for (size_t k = 0; k < N; ++k) {
            Value* f = b.build_fpext_f64_f32(
                b.build_load_indexed(Type::f32(), f32_data, base_i64, 4, static_cast<int32_t>(k * 4)));
            f32_boxed[k] = prop_lowering_.box_number(b, f);
        }
        b.build_br(bb_store, f32_boxed);

        b.position_at_end(bb_src_ta_f64);
        Value* f64_data = prop_lowering_.load_typed_array_data(b, arr_ptr);
        std::vector<Value*> f64_boxed(N, nullptr);
        for (size_t k = 0; k < N; ++k) {
            Value* f = b.build_load_indexed(Type::f64(), f64_data, base_i64, 8, static_cast<int32_t>(k * 8));
            f64_boxed[k] = prop_lowering_.box_number(b, f);
        }
        b.build_br(bb_store, f64_boxed);

        b.position_at_end(bb_store);
        for (size_t k = 0; k < N; ++k) {
            if (dst_kind == ElemRunPeerKind::DenseArray) {
                prop_lowering_.store_proven_dense_array_elem(b, dst_block, dst_head, get_steps[k].dst_elem_idx,
                                                             store_params[k]);
            } else {
                Value* base = prop_lowering_.slot_base(b, dst_ptr, dst_slot_words[k]);
                b.build_store_indexed(Type::i64(), base, dst_slot_words[k], 8, kBronzeObjSlotsOffset, store_params[k]);
            }
        }
        // Every `store_params[k]` is a boxed Number, so no write barrier is needed.
        b.build_br(bb_merge);

        // Slow path: lower the slice in helper-only mode (`run_once_ = true`).
        b.position_at_end(bb_slow);
        invalidate_dense_array_proofs();
        const bool saved_run_once = run_once_;
        run_once_ = true;
        for (size_t j = inst_idx; j < committed_end; ++j) {
            apply_loc(insts[j]);
            if (!lower_instruction(insts[j], b, fn, val_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
                run_once_ = saved_run_once;
                return false;
            }
        }
        run_once_ = saved_run_once;
        b.build_br(bb_merge);

        b.position_at_end(bb_merge);
        invalidate_dense_array_proofs();
        inst_idx = committed_end - 1;
        return true;
    }

    return false;
}

} // namespace il2mir

// Literals and construction (il_op.h): `object.literal`, `array.literal`, and
// the speculated function's forms of `new` — `fn.constructs`,
// `fn.instance_shape`, `create.this` — and of a property add, `shape.store`.
//
// An object literal is made with its final shape: the runtime remembers it
// per literal and per thread (`__bronze_literal_shapes`, il_abi.h
// kLiteralShapesSymbol) once one generic build showed it is a plain shape of
// boxed data slots 0..n-1, all inline. The fast path bump-allocates an object
// of that shape and stores the values; it is taken while the TLS block's
// empty root of the literal's size is the root the shape was found from,
// which names the current realm's Object.prototype. Any other time the
// runtime builds the object the generic way (bronze_object_literal).
//
// An array literal is a fresh array of its length (the same allocation
// `create.array` makes, whose elements block holds at least that many) with
// its elements stored in place.

#include <algorithm>

#include <brass/mir/module.hpp>

#include "il_abi.h"
#include "il_lowering.h"
#include "il_lowering_ops.h"

namespace il2mir {

namespace {

constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kRealmMask = ~uint64_t{0} << BRONZE_ABI_FN_REALM_SHIFT;
constexpr int kVetShift = (BRONZE_ABI_FN_CTOR_VETTED_OFFSET - BRONZE_ABI_FN_FLAGS_WORD_OFFSET) * 8;

Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

}  // namespace

bool is_literal_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::ObjectLiteral:
        case BronzeOp::ArrayLiteral:
        case BronzeOp::FnConstructs:
        case BronzeOp::FnInstanceShape:
        case BronzeOp::CreateThis:
        case BronzeOp::ShapeStore:
        case BronzeOp::ProtoCell:
        case BronzeOp::ProtoPublish:
        case BronzeOp::MemberCell:
        case BronzeOp::MemberPublish:
        case BronzeOp::ConstructResult:
            return true;
        default:
            return false;
    }
}

bool IlLowering::lower_literal_instruction(const BronzeInstruction& inst, Builder& b,
                                           std::unordered_map<uint32_t, Value*>& val_map, Value*& res_val) {
    std::vector<Value*> opd;
    opd.reserve(inst.operands.size());
    for (uint32_t id : inst.operands) {
        Value* v = get_val_by_id(id, b, val_map);
        if (v == nullptr) return inst_error(inst, "an operand of a literal or construction form is not defined here");
        opd.push_back(ensure_type(v, Type::i64(), b));
    }
    auto need = [&](size_t n) {
        return opd.size() >= n ? true : inst_error(inst, "a construction form with too few operands");
    };

    switch (inst.op) {
        case BronzeOp::ObjectLiteral: {
            if (inst.imm_i64 < 0 || static_cast<size_t>(inst.imm_i64) >= options_.object_literals.size()) {
                return inst_error(inst, "object.literal names no literal of the module");
            }
            const TranslatorOptions::ObjectLiteralMeta& lit = options_.object_literals[inst.imm_i64];
            if (lit.keys.size() != opd.size()) return inst_error(inst, "object.literal with the wrong value count");
            // The root create.object would start from (il_alloc_lowering.cpp):
            // the default one up to its capacity, else one sized to the want.
            const uint32_t inl = lit.inline_slots <= BRONZE_ABI_OBJ_INLINE_SLOTS ? 0u
                                 : std::min<uint32_t>(lit.inline_slots, BRONZE_ABI_OBJ_OVERFLOW_SLOT);
            const uint32_t slots = inl == 0 ? BRONZE_ABI_OBJ_INLINE_SLOTS : inl;
            const int32_t root_off = inl == 0 ? BRONZE_TLS_PLAIN_SHAPE_OFF
                                              : static_cast<int32_t>(BRONZE_TLS_PLAIN_SHAPES_OFF + 8 * inl);
            Function* fn = b.current_block()->parent();
            const std::string p = "objlit_" + std::to_string(fn->next_block_id());
            BasicBlock* cur = b.current_block();
            BasicBlock* fast = b.append_block(p + "_fast");
            BasicBlock* slow = b.append_block(p + "_slow");
            BasicBlock* merge = b.append_block(p + "_merge");
            Value* result = b.add_block_param(merge, Type::i64());
            slow->set_cold();
            b.position_at_end(cur);

            Value* cache = b.build_add(module_data_addr(b, std::string(kLiteralShapesSymbol)),
                                       b.build_iconst_i64(inst.imm_i64 * 16));
            Value* root = with_alias_class(b.build_load(Type::i64(), cache, 0), kRuntimeWordAliasClass);
            Value* shape = with_alias_class(b.build_load(Type::i64(), cache, 8), kRuntimeWordAliasClass);
            Value* tls_root = b.build_load(Type::i64(), tls_block(b), root_off);
            Value* ok = b.build_and(b.build_eq(root, tls_root), b.build_ne(root, b.build_iconst_i64(0)));
            // The positions under a key this module made eligible for a
            // double slot (il.h Module::numberSlotKeys): the runtime caches
            // a shape keeping such a slot a double one only at these
            // (`numbers`, runtime/literal.cpp), and a double slot takes only
            // a Number, so the fast path is for an object whose every value
            // there that is not known a Number is one.
            uint64_t numbers = 0;
            std::vector<size_t> tested;
            for (size_t i = 0; i < opd.size() && i < BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT; ++i) {
                const bool eligible = std::find(options_.number_slot_keys.begin(), options_.number_slot_keys.end(),
                                                lit.keys[i]) != options_.number_slot_keys.end();
                if (!eligible) continue;
                numbers |= uint64_t{1} << i;
                if (boxed_f64_source(opd[i]) == nullptr) tested.push_back(i);
            }
            if (!tested.empty()) {
                BasicBlock* here = b.current_block();
                BasicBlock* check = b.append_block(p + "_repr");
                b.position_at_end(here);
                b.build_br_if(ok, check, slow);
                b.position_at_end(check);
                Value* doubles = with_alias_class(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_DOUBLESLOTS_OFFSET),
                                                  kRuntimeWordAliasClass);
                Value* refused = b.build_iconst_i64(0);
                for (size_t i : tested) {
                    Value* not_number = b.build_zext_i64(
                        b.build_ugt(opd[i], b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits))));
                    refused = b.build_or(refused, b.build_shl(not_number, b.build_iconst_i64(static_cast<int64_t>(i))));
                }
                ok = b.build_eq(b.build_and(refused, doubles), b.build_iconst_i64(0));
            }
            Value* obj = alloc_lowering_.bump_plain_object(b, shape, ok, slow, fast, slots);
            Value* ptr = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
            for (size_t i = 0; i < opd.size(); ++i) {
                Value* addr = b.build_add(ptr, b.build_iconst_i64(kBronzeObjSlotsOffset + 8 * static_cast<int64_t>(i)));
                const bool primitive =
                    is_primitive_value(inst.operands[i]) || prop_lowering_.is_known_non_ref(opd[i]);
                prop_lowering_.store_value(b, addr, opd[i], kObjectSlotAliasClass, !primitive);
            }
            b.build_br(merge, {obj});

            // The values, then the runtime key ids, one word each.
            b.position_at_end(slow);
            std::vector<Value*> staged = opd;
            for (uint32_t k : lit.keys) staged.push_back(b.build_zext_i64(get_key_id(b, k)));
            Value* argv = stage_argv(b, staged);
            b.build_br(merge, {b.build_call("bronze_object_literal", Type::i64(),
                                            {cache, b.build_iconst_i32(static_cast<int32_t>(inl)),
                                             b.build_iconst_i32(static_cast<int32_t>(opd.size())), argv,
                                             b.build_iconst_i64(static_cast<int64_t>(numbers))})});
            b.position_at_end(merge);
            res_val = result;
            return true;
        }
        case BronzeOp::ArrayLiteral: {
            const uint32_t n = static_cast<uint32_t>(opd.size());
            Value* arr = alloc_lowering_.lower_create_array(b, b.build_iconst_i32(static_cast<int32_t>(n)), n);
            if (n != 0) {
                Value* ptr = b.build_and(arr, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
                Value* elems = with_alias_class(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET),
                                                kArrayHeaderAliasClass);
                Value* base = b.build_and(elems, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
                for (uint32_t i = 0; i < n; ++i) {
                    Value* addr = b.build_add(base, b.build_iconst_i64(kBronzeHdrBytes + 8 * static_cast<int64_t>(i)));
                    const bool primitive =
                        is_primitive_value(inst.operands[i]) || prop_lowering_.is_known_non_ref(opd[i]);
                    prop_lowering_.store_value(b, addr, opd[i], kBoxedElementAliasClass, !primitive);
                }
                // A literal whose every element is a Number is born NUMBERS
                // (runtime/array.h): n compares here, where the first read
                // would otherwise call the claim to scan it. Longer literals
                // are left to the claim.
                constexpr uint32_t kBornNumbersMax = 16;
                const bool boxed = std::all_of(opd.begin(), opd.end(), [](Value* v) { return v->type() == Type::i64(); });
                if (n <= kBornNumbersMax && boxed) {
                    Value* all = nullptr;
                    for (uint32_t i = 0; i < n; ++i) {
                        Value* is_num = b.build_ult(opd[i], b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits + 1)));
                        all = all == nullptr ? is_num : b.build_and(all, is_num);
                    }
                    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
                    Value* numbers = b.build_select(all, b.build_iconst_i32(BRONZE_ABI_ARRAY_BIT_NUMBERS), b.build_iconst_i32(0));
                    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET, b.build_or(bits, numbers));
                }
            }
            res_val = arr;
            return true;
        }
        // bool = fn.constructs %f: %f (a function, by a dominating fn.code_is)
        // is of this realm, vetted for `new` (bronze_construct took its plain
        // path for it) and has its instance shape.
        case BronzeOp::FnConstructs: {
            if (!need(1)) return false;
            Value* ptr = object_address(b, opd[0]);
            Value* word = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_FLAGS_WORD_OFFSET);
            Value* realm = b.build_shl(b.build_load(Type::i64(), tls_block(b), BRONZE_TLS_REALM_SLOT_OFF),
                                       b.build_iconst_i64(BRONZE_ABI_FN_REALM_SHIFT));
            const uint64_t mask = kRealmMask | (uint64_t{0xFF} << kVetShift);
            Value* masked = b.build_and(word, b.build_iconst_i64(static_cast<int64_t>(mask)));
            Value* vetted = b.build_eq(masked, b.build_or(realm, b.build_iconst_i64(int64_t{1} << kVetShift)));
            Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET);
            res_val = b.build_and(vetted, b.build_ne(shape, b.build_iconst_i64(0)));
            return true;
        }
        // ptr = fn.instance_shape %f: the instance shape word.
        case BronzeOp::FnInstanceShape: {
            if (!need(1)) return false;
            res_val = b.build_load(Type::i64(), object_address(b, opd[0]), BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET);
            return true;
        }
        // dynamic = create.this %f, %shape: a plain object of %f's instance
        // shape %shape, bump-allocated, else made by bronze_create_this.
        case BronzeOp::CreateThis: {
            if (!need(2)) return false;
            Function* fn = b.current_block()->parent();
            const std::string p = "newthis_" + std::to_string(fn->next_block_id());
            BasicBlock* cur = b.current_block();
            BasicBlock* fast = b.append_block(p + "_fast");
            BasicBlock* slow = b.append_block(p + "_slow");
            BasicBlock* merge = b.append_block(p + "_merge");
            Value* result = b.add_block_param(merge, Type::i64());
            slow->set_cold();
            b.position_at_end(cur);
            Value* obj = alloc_lowering_.bump_plain_object(b, opd[1], b.build_ne(opd[1], b.build_iconst_i64(0)),
                                                           slow, fast);
            b.build_br(merge, {obj});
            b.position_at_end(slow);
            b.build_br(merge, {b.build_call("bronze_create_this", Type::i64(), {opd[0]})});
            b.position_at_end(merge);
            res_val = result;
            return true;
        }
        // shape.store %obj, %shape: the object's shape word.
        case BronzeOp::ShapeStore: {
            if (!need(2)) return false;
            with_alias_class(b.build_store(Type::i64(), object_address(b, opd[0]), BRONZE_ABI_OBJ_SHAPE_OFFSET, opd[1]),
                             kShapeWordAliasClass);
            return true;
        }
        // bool = proto.cell <c>: the cell's stamp is the thread's epoch.
        case BronzeOp::ProtoCell: {
            Value* cells = module_data_addr(b, std::string(kProtoCellsSymbol));
            Value* stamp = with_alias_class(b.build_load(Type::i64(), cells, static_cast<int32_t>(inst.imm_i64 * 8)),
                                            kRuntimeWordAliasClass);
            Value* epoch = b.build_load(Type::i64(), tls_block(b), BRONZE_TLS_PROTO_EPOCH_OFF);
            res_val = b.build_eq(stamp, epoch);
            return true;
        }
        // proto.publish <c>: bronze_transition_publish with the cell's two
        // shapes as the shape cells hold them now.
        case BronzeOp::ProtoPublish: {
            if (inst.imm_i64 < 0 || static_cast<size_t>(inst.imm_i64) >= options_.proto_cells.size()) {
                return inst_error(inst, "proto.publish names no cell of the module");
            }
            const TranslatorOptions::ProtoCellMeta& c = options_.proto_cells[inst.imm_i64];
            Value* cells = module_data_addr(b, std::string(kProtoCellsSymbol));
            Value* cell = b.build_add(cells, b.build_iconst_i64(inst.imm_i64 * 8));
            Value* shapes = module_data_addr(b, std::string(kShapeCellsSymbol));
            Value* from = b.build_load(Type::i64(), shapes, static_cast<int32_t>(c.from_cell * 8));
            Value* to = b.build_load(Type::i64(), shapes, static_cast<int32_t>(c.to_cell * 8));
            b.build_call("bronze_transition_publish", Type::void_type(),
                         {cell, from, get_key_id(b, c.key_index), to, b.build_iconst_i32(c.slot)});
            return true;
        }
        // ptr = member.cell <c>: the cell's shape word.
        case BronzeOp::MemberCell: {
            Value* cells = module_data_addr(b, std::string(kMemberCellsSymbol));
            res_val = with_alias_class(b.build_load(Type::i64(), cells, static_cast<int32_t>(inst.imm_i64 * 16)),
                                       kRuntimeWordAliasClass);
            return true;
        }
        // member.publish %recv, <c>: bronze_member_cell_publish with the
        // cell's function's code word (0 for an absent cell) and its group.
        case BronzeOp::MemberPublish: {
            if (!need(1)) return false;
            if (inst.imm_i64 < 0 || static_cast<size_t>(inst.imm_i64) >= options_.member_cells.size()) {
                return inst_error(inst, "member.publish names no member cell of the module");
            }
            const TranslatorOptions::MemberCellMeta& c = options_.member_cells[inst.imm_i64];
            const bool absent = c.kind == kMemberKindAbsent;
            if (!absent && (inst.index >= resolved_names_.size() || resolved_names_[inst.index].empty())) {
                return inst_error(inst, "member.publish names no function");
            }
            Value* cells = module_data_addr(b, std::string(kMemberCellsSymbol));
            Value* cell = b.build_add(cells, b.build_iconst_i64(inst.imm_i64 * 16));
            Value* group = b.build_add(cells, b.build_iconst_i64(int64_t{c.group} * 16));
            Value* code = absent ? b.build_iconst_i64(0) : b.build_func_addr("__wrapper_" + resolved_names_[inst.index]);
            b.build_call("bronze_member_cell_publish", Type::void_type(),
                         {opd[0], get_key_id(b, c.key_index), cell, code, b.build_iconst_i32(static_cast<int32_t>(c.kind)),
                          group, b.build_iconst_i32(static_cast<int32_t>(c.group_size))});
            return true;
        }
        // dynamic = construct.result %r, %this.
        //   xor ; cmp 1<<48 ; cmovb
        case BronzeOp::ConstructResult: {
            if (!need(2)) return false;
            Value* is_obj = b.build_ult(b.build_xor(opd[0], b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits))),
                                        b.build_iconst_i64(int64_t{1} << 48));
            res_val = b.build_select(is_obj, opd[0], opd[1]);
            return true;
        }
        default:
            return inst_error(inst, "not a literal or construction form");
    }
}

}  // namespace il2mir

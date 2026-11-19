// The speculated function's low-level forms (il_op.h, "speculation: the
// low-level forms S is written in"): each one machine-level step with no
// fallback. The unchecked ones read through their operand as the dominating
// branch that licensed them established it (an object, an array, an index in
// bounds); they test nothing themselves.
//
// The Value encoding they read (bronze_abi_layout.h): a NaN-boxed word, the
// tag in its top 16 bits. A Number in its double form is any word at most
// BRONZE_ABI_NUMBER_MAX_BITS; the Int32 box is BRONZE_ABI_TAG_INT32 over the
// int32's low 32 bits, and is not counted by `is.number`. An object's address
// is its word with the object tag XORed away, below 2^48 exactly when the tag
// was the object tag (il_abi.h object_address).

#include "il_abi.h"
#include "il_lowering.h"
#include "il_lowering_ops.h"
#include "il_prop_stubs.h"

namespace il2mir {

namespace {

constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kNegativeZeroBits = 0x8000000000000000ULL;
// A function object's header word, low half: its kind over the object tag
// (runtime/heap.h HeapObjectHeader: tag, then flags).
constexpr int32_t kFunctionHeaderLow =
    static_cast<int32_t>((uint32_t{BRONZE_ABI_OBJ_FLAGS_FUNCTION} << 16) | uint32_t{BRONZE_ABI_TAG_OBJECT});
static_assert(BRONZE_ABI_OBJ_FLAGS_OFFSET == 2, "the kind flags are the header word's second half-word");
constexpr int32_t kArrayHeaderLow =
    static_cast<int32_t>((uint32_t{BRONZE_ABI_OBJ_FLAGS_ARRAY} << 16) | uint32_t{BRONZE_ABI_TAG_OBJECT});
constexpr int32_t kValueBlockHeaderLow =
    static_cast<int32_t>((uint32_t{BRONZE_ABI_OBJ_FLAGS_VALUE_BLOCK} << 16) | uint32_t{BRONZE_ABI_TAG_OBJECT});

Value* iconst64(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

// Whether the double `f` is an int32 other than -0: its truncation, cut to
// 32 bits and converted back, is `f` again. fptosi.i64 is INT64_MIN for a NaN
// or a value outside the i64 range (mir_reference.md), a fraction does not
// survive the truncation, and an integral value outside int32 does not
// survive the cut, so each compares unequal; -0 compares equal to 0 and is
// refused by its bits.
Value* double_is_int32(Builder& b, Value* f) {
    Value* back = b.build_sitofp_f64_i32(b.build_trunc_i32(b.build_fptosi_i64(f)));
    return b.build_and(b.build_eq(back, f), b.build_ne(b.build_bitcast_i64_f64(f), iconst64(b, kNegativeZeroBits)));
}

// The address `slot` of the plain object at `ptr` is at: inline below
// BRONZE_ABI_OBJ_OVERFLOW_SLOT, else in the overflow block, whose Values
// start after its header.
Value* slot_address(Builder& b, Value* ptr, int64_t slot) {
    if (slot < kBronzeObjOverflowSlot) {
        return b.build_add(ptr, b.build_iconst_i64(kBronzeObjSlotsOffset + 8 * slot));
    }
    Value* block = b.build_and(with_alias_class(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset),
                                                kShapeWordAliasClass),
                               iconst64(b, kPayloadMask));
    return b.build_add(block, b.build_iconst_i64(kBronzeHdrBytes + 8 * (slot - kBronzeObjOverflowSlot)));
}


}  // namespace

bool is_spec_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::LoadShape:
        case BronzeOp::ShapeCell:
        case BronzeOp::PtrEq:
        case BronzeOp::ShapeIn:
        case BronzeOp::ShapeHasOwn:
        case BronzeOp::ShapeLacks:
        case BronzeOp::ShapeProto:
        case BronzeOp::IsObject:
        case BronzeOp::SlotLoad:
        case BronzeOp::SlotStore:
        case BronzeOp::IsI32:
        case BronzeOp::UnboxI32:
        case BronzeOp::UnboxF64:
        case BronzeOp::I32ToF64:
        case BronzeOp::AddWouldOvf:
        case BronzeOp::SubWouldOvf:
        case BronzeOp::MulWouldOvf:
        case BronzeOp::FnCodeIs:
        case BronzeOp::FnEnv:
        case BronzeOp::ArrayLength:
        case BronzeOp::ElemLoad:
        case BronzeOp::ElemStore:
        case BronzeOp::InBounds:
        case BronzeOp::IsHole:
        case BronzeOp::IsUninit:
        case BronzeOp::IsArray:
        case BronzeOp::TypedLength:
        case BronzeOp::TypedLoad:
        case BronzeOp::TypedStore:
        case BronzeOp::FnNativeIs:
        case BronzeOp::MathFn:
            return true;
        default:
            return false;
    }
}

bool IlLowering::lower_spec_instruction(const BronzeInstruction& inst, Builder& b,
                                        std::unordered_map<uint32_t, Value*>& val_map, Value*& res_val) {
    std::vector<Value*> opd;
    opd.reserve(inst.operands.size());
    for (uint32_t id : inst.operands) {
        Value* v = get_val_by_id(id, b, val_map);
        if (v == nullptr) return inst_error(inst, "an operand of a speculation form is not defined here");
        opd.push_back(v);
    }
    auto need = [&](size_t n) {
        return opd.size() >= n ? true : inst_error(inst, "a speculation form with too few operands");
    };
    auto bits = [&](size_t i) { return ensure_type(opd[i], Type::i64(), b); };
    auto i32 = [&](size_t i) { return ensure_type(opd[i], Type::i32(), b); };

    switch (inst.op) {
        // ptr = load.shape %obj: the shape word at BRONZE_ABI_OBJ_SHAPE_OFFSET.
        //   xor rax, OBJECT_TAG<<48 ; mov rax, [rax+8]
        case BronzeOp::LoadShape: {
            if (!need(1)) return false;
            res_val = with_alias_class(
                b.build_load(Type::i64(), object_address(b, bits(0)), BRONZE_ABI_OBJ_SHAPE_OFFSET),
                kShapeWordAliasClass);
            return true;
        }
        // ptr = shape.cell <n>: word n of the module's shape cells, which only
        // the runtime writes (bronze_register_shape_cells, once).
        //   mov rax, [__bronze_shape_cells + delta + 8n]
        case BronzeOp::ShapeCell: {
            Value* cells = module_data_addr(b, std::string(kShapeCellsSymbol));
            res_val = with_alias_class(b.build_load(Type::i64(), cells, static_cast<int32_t>(inst.imm_i64 * 8)),
                                       kRuntimeWordAliasClass);
            return true;
        }
        // bool = ptr.eq %a, %b:  cmp ; sete
        case BronzeOp::PtrEq: {
            if (!need(2)) return false;
            res_val = b.build_eq(bits(0), bits(1));
            return true;
        }
        // bool = shape.in %shape, %c1, ..., %cn:  a compare per cell, or'd
        //   cmp ; sete ; cmp ; sete ; or ...
        case BronzeOp::ShapeIn: {
            if (!need(3)) return false;
            Value* shape = bits(0);
            Value* any = b.build_eq(shape, bits(1));
            for (size_t i = 2; i < opd.size(); ++i) any = b.build_or(any, b.build_eq(shape, bits(i)));
            res_val = any;
            return true;
        }
        // bool = shape.has_own %shape, <slot, write>, "k": the shape's
        // slot-key table (built by the runtime on the first ask), then its
        // entry for the slot against the key's index and the attribute bits
        // the access needs; the write form also wants the shape no prototype's.
        //   t = [shape+96] ; t == 0 ? t = bronze_shape_slot_keys(shape) (cold)
        //   in = slot < [t] ; e = in ? [t + 8 + 8*slot] : 0
        //   r = (e & mask) == ((key_map[k] + 1) << 2 | bits)
        case BronzeOp::ShapeHasOwn: {
            if (!need(1)) return false;
            const int64_t slot = inst.imm_i64 >> kHasOwnSlotShift;
            const bool write = (inst.imm_i64 & kHasOwnWrite) != 0;
            const bool number = (inst.imm_i64 & kHasOwnNumber) != 0;
            Value* shape = bits(0);
            BasicBlock* cur = b.current_block();
            const std::string prefix = "hasown_" + std::to_string(cur->parent()->next_block_id());
            BasicBlock* build = b.append_block(prefix + "_build");
            build->set_cold();
            BasicBlock* have = b.append_block(prefix + "_have");
            Value* table = b.add_block_param(have, Type::i64());
            b.position_at_end(cur);
            Value* word = with_alias_class(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_SLOTKEYS_OFFSET),
                                           kRuntimeWordAliasClass);
            b.build_br_if(b.build_eq(word, iconst64(b, 0)), build, {}, have, {word});
            b.position_at_end(build);
            b.build_br(have, {b.build_call("bronze_shape_slot_keys", Type::i64(), {shape})});
            b.position_at_end(have);
            Value* count = with_alias_class(b.build_load(Type::i64(), table, 0), kRuntimeWordAliasClass);
            Value* in = b.build_ult(iconst64(b, static_cast<uint64_t>(slot)), count);
            // Past the count the entry is read as 0 through the table's own
            // first word, which is in bounds: no branch.
            Value* at = b.build_select(in, b.build_add(table, iconst64(b, 8 + 8 * static_cast<uint64_t>(slot))), table);
            Value* entry = b.build_select(
                in, with_alias_class(b.build_load(Type::i64(), at, 0), kRuntimeWordAliasClass), iconst64(b, 0));
            // The bits the mode asks for, and the ones it does not care about:
            // a read takes either representation (a double slot's word is the
            // Number's Value), a Number read only a double slot, a store only
            // a boxed slot, and a Number's store either.
            const uint64_t need_bits = BRONZE_ABI_SLOTKEY_DATA | (write ? BRONZE_ABI_SLOTKEY_WRITABLE : 0u) |
                                       (number && !write ? BRONZE_ABI_SLOTKEY_NUMBER : 0u);
            const uint64_t ignored = (write ? 0u : BRONZE_ABI_SLOTKEY_WRITABLE) |
                                     (number == write ? BRONZE_ABI_SLOTKEY_NUMBER : 0u);
            const uint64_t mask = ~uint64_t{ignored};
            Value* key = b.build_zext_i64(get_key_id(b, inst.index));
            Value* want = b.build_or(b.build_shl(b.build_add(key, iconst64(b, 1)),
                                                 iconst64(b, BRONZE_ABI_SLOTKEY_KEY_SHIFT)),
                                     iconst64(b, need_bits));
            Value* match = b.build_eq(ignored == 0 ? entry : b.build_and(entry, iconst64(b, mask)), want);
            if (write) {
                Value* proto = with_alias_class(
                    b.build_load(Type::i8(), shape, BRONZE_ABI_SHAPE_USEDPROTO_OFFSET), kRuntimeWordAliasClass);
                match = b.build_and(match, b.build_eq(b.build_zext_i64(proto), iconst64(b, 0)));
            }
            res_val = match;
            return true;
        }
        // bool = shape.lacks %obj, %shape, "k": the shape's absent cache, in
        // front of its slot-key table (bronze_abi_layout.h
        // BRONZE_ABI_SLOTKEY_LACKS_ENTRIES), answers when the key's home or
        // alternate entry names the key at the thread's current epoch; an
        // entry for the key marked FOUND answers no, and anything else asks
        // the runtime, which fills an entry with its answer (cold).
        //   t = [shape+96] ; t == 0 ? t = bronze_shape_slot_keys(shape) (cold)
        //   k = key_map[k] ; l = t - LACKS_BYTES
        //   for e in (l + 16 * HOME(k), l + 16 * ALT(k)):
        //     [e+8] == [tls] && [e] == k + 1 ? 1
        //     : [e+8] == [tls] && [e] == (k + 1 | FOUND) ? 0
        //   bronze_shape_lacks(obj, k) (cold)
        case BronzeOp::ShapeLacks: {
            if (!need(2)) return false;
            Value* obj = bits(0);
            Value* shape = bits(1);
            BasicBlock* cur = b.current_block();
            const std::string prefix = "lacks_" + std::to_string(cur->parent()->next_block_id());
            BasicBlock* build = b.append_block(prefix + "_build");
            build->set_cold();
            BasicBlock* have = b.append_block(prefix + "_have");
            Value* table = b.add_block_param(have, Type::i64());
            BasicBlock* home_found = b.append_block(prefix + "_home_found");
            BasicBlock* alt = b.append_block(prefix + "_alt");
            BasicBlock* alt_found = b.append_block(prefix + "_alt_found");
            BasicBlock* miss = b.append_block(prefix + "_miss");
            miss->set_cold();
            BasicBlock* done = b.append_block(prefix + "_done");
            Value* answer = b.add_block_param(done, Type::i32());
            b.position_at_end(cur);
            Value* word = with_alias_class(b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_SLOTKEYS_OFFSET),
                                           kRuntimeWordAliasClass);
            b.build_br_if(b.build_eq(word, iconst64(b, 0)), build, {}, have, {word});
            b.position_at_end(build);
            b.build_br(have, {b.build_call("bronze_shape_slot_keys", Type::i64(), {shape})});
            b.position_at_end(have);
            Value* key = get_key_id(b, inst.index);
            Value* key64 = b.build_zext_i64(key);
            Value* lacks = b.build_sub(table, iconst64(b, BRONZE_ABI_SLOTKEY_LACKS_BYTES));
            Module* mod = b.current_block()->parent()->parent();
            Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                             : b.build_call("bronze_tls_block_addr", Type::i64(), {});
            Value* epoch = b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF);
            Value* tag = b.build_add(key64, iconst64(b, 1));
            Value* found_tag = b.build_or(tag, iconst64(b, BRONZE_ABI_SLOTKEY_LACKS_FOUND));
            // The entry at bucket index `bucket`: absent -> 1, else through
            // `found`: found -> 0, anything else on to `next`.
            auto probe = [&](Value* bucket, BasicBlock* found, BasicBlock* next) {
                Value* entry = b.build_add(lacks, b.build_shl(bucket, iconst64(b, 4)));
                Value* word0 = b.build_load(Type::i64(), entry, 0);
                Value* current = b.build_eq(b.build_load(Type::i64(), entry, 8), epoch);
                b.build_br_if(b.build_and(b.build_eq(word0, tag), current), done, {b.build_iconst_i32(1)}, found, {});
                b.position_at_end(found);
                b.build_br_if(b.build_and(b.build_eq(word0, found_tag), current), done, {b.build_iconst_i32(0)}, next,
                              {});
            };
            const int64_t bucket_mask = BRONZE_ABI_SLOTKEY_LACKS_ENTRIES - 1;
            probe(b.build_and(key64, iconst64(b, bucket_mask)), home_found, alt);
            b.position_at_end(alt);
            Value* hash = b.build_lshr(b.build_mul(key64, iconst64(b, BRONZE_ABI_SLOTKEY_LACKS_HASH)),
                                       iconst64(b, BRONZE_ABI_SLOTKEY_LACKS_HASH_SHIFT));
            probe(b.build_and(hash, iconst64(b, bucket_mask)), alt_found, miss);
            b.position_at_end(miss);
            Value* asked = b.build_call("bronze_shape_lacks", Type::i64(), {obj, key});
            b.build_br(done, {b.build_ne(asked, iconst64(b, 0))});
            b.position_at_end(done);
            res_val = answer;
            return true;
        }
        // dynamic = shape.proto %shape: the root's prototype Value, words
        // only the runtime writes.
        //   mov rax, [shape+24] ; mov rax, [rax+32]
        case BronzeOp::ShapeProto: {
            if (!need(1)) return false;
            Value* root = with_alias_class(b.build_load(Type::i64(), bits(0), BRONZE_ABI_SHAPE_ROOT_OFFSET),
                                           kRuntimeWordAliasClass);
            res_val = with_alias_class(b.build_load(Type::i64(), root, BRONZE_ABI_SHAPE_PROTO_OFFSET),
                                       kRuntimeWordAliasClass);
            return true;
        }
        // bool = is.object %v:  xor rax, OBJECT_TAG<<48 ; cmp rax, 1<<48 ; setb
        case BronzeOp::IsObject: {
            if (!need(1)) return false;
            res_val = is_object_address(b, object_address(b, bits(0)));
            return true;
        }
        // dynamic = slot.load %obj, <slot>: inline slot: one load at
        // ptr + 24 + 8*slot; overflow slot: the block word at ptr + 16,
        // masked, then the load at block + 8 + 8*(slot - 62). A double slot's
        // (kSlotLoadNumber) is loaded as the double and boxed as its bits,
        // canonical already (runtime/slot_repr.h), so an unbox is the double.
        case BronzeOp::SlotLoad: {
            if (!need(1)) return false;
            Value* addr = slot_address(b, object_address(b, bits(0)), inst.imm_i64 & ~kSlotLoadNumber);
            if ((inst.imm_i64 & kSlotLoadNumber) != 0) {
                res_val = b.build_bitcast_i64_f64(
                    with_alias_class(b.build_load(Type::f64(), addr, 0), kObjectSlotAliasClass));
                return true;
            }
            res_val = with_alias_class(b.build_load(Type::i64(), addr, 0), kObjectSlotAliasClass);
            return true;
        }
        // slot.store %obj, <slot>, %v: the store at slot.load's address, then
        // for a value above NUMBER_MAX the write barrier on that address.
        case BronzeOp::SlotStore: {
            if (!need(2)) return false;
            Value* addr = slot_address(b, object_address(b, bits(0)), inst.imm_i64);
            Value* v = bits(1);
            const bool primitive = is_primitive_value(inst.operands[1]) || prop_lowering_.is_known_non_ref(v);
            prop_lowering_.store_value(b, addr, v, kObjectSlotAliasClass, !primitive);
            return true;
        }
        // bool = is.i32 %v: the Int32 box, or a double that is integral, in
        // int32 range and not -0. Read as a double, every other value's bits
        // are a NaN, which the double's test refuses, so the test needs no
        // range check in front of it; and a box made of a double here (a
        // box.f64, a double slot's or a NUMBERS element's load) is that
        // double, whose bits are never the Int32 tag's.
        //   tag = v >> 48 == INT32 ; ok = tag | double_is_int32(v as f64)
        case BronzeOp::IsI32: {
            if (!need(1)) return false;
            if (Value* f = boxed_f64_source(opd[0])) {
                res_val = double_is_int32(b, f);
                return true;
            }
            Value* v = bits(0);
            Value* tagged_i32 = b.build_eq(b.build_lshr(v, b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT)),
                                           b.build_iconst_i64(BRONZE_ABI_TAG_INT32));
            res_val = b.build_or(tagged_i32, double_is_int32(b, b.build_bitcast_f64_i64(v)));
            return true;
        }
        // i32 = unbox.i32.unchecked %v: the Int32 box's low half, or the
        // double's truncation (exact on is.i32's true edge). The double is
        // read as 0 for an Int32 box, so the conversion never sees a NaN.
        //   tag = v >> 48 == INT32 ; f = tag ? 0 : v ; r = tag ? (i32)v : (i32)cvttsd2si64 f
        // A box made of a double here is that double's truncation.
        case BronzeOp::UnboxI32: {
            if (!need(1)) return false;
            if (Value* f = boxed_f64_source(opd[0])) {
                res_val = b.build_trunc_i32(b.build_fptosi_i64(f));
                return true;
            }
            Value* v = bits(0);
            Value* tagged_i32 = b.build_eq(b.build_lshr(v, b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT)),
                                           b.build_iconst_i64(BRONZE_ABI_TAG_INT32));
            Value* f = b.build_bitcast_f64_i64(b.build_select(tagged_i32, b.build_iconst_i64(0), v));
            res_val = b.build_select(tagged_i32, b.build_trunc_i32(v), b.build_trunc_i32(b.build_fptosi_i64(f)));
            return true;
        }
        // f64 = unbox.f64.unchecked %v: the bits as a double (movq).
        case BronzeOp::UnboxF64: {
            if (!need(1)) return false;
            // A box made of a double here (box.f64, a NUMBERS element) is
            // that double.
            if (Value* f = boxed_f64_source(opd[0])) {
                res_val = f;
                return true;
            }
            res_val = b.build_bitcast_f64_i64(bits(0));
            return true;
        }
        // f64 = i32.to_f64 %i: cvtsi2sd.
        case BronzeOp::I32ToF64: {
            if (!need(1)) return false;
            res_val = b.build_sitofp_f64_i32(i32(0));
            return true;
        }
        // bool = add/sub.would_ovf %a, %b: brass's 32-bit overflow flag
        // (add ; seto), the add's result itself unused.
        case BronzeOp::AddWouldOvf: {
            if (!need(2)) return false;
            res_val = b.build_sadd_overflow(i32(0), i32(1));
            return true;
        }
        case BronzeOp::SubWouldOvf: {
            if (!need(2)) return false;
            res_val = b.build_ssub_overflow(i32(0), i32(1));
            return true;
        }
        // bool = mul.would_ovf %a, %b: the 32-bit overflow flag, or a zero
        // product with a negative operand (-0):
        //   imul ; seto | (a*b == 0 & (a|b) < 0)
        // A constant factor k decides the -0 half: k > 0 never makes one (a
        // zero product needs a == 0, which is not negative), k < 0 makes one
        // exactly when a == 0, k == 0 when a < 0.
        case BronzeOp::MulWouldOvf: {
            if (!need(2)) return false;
            Value* a = i32(0);
            Value* c = i32(1);
            Value* zero = b.build_iconst_i32(0);
            const Instruction* ka = a->is_instruction() ? a->defining_instruction() : nullptr;
            const Instruction* kc = c->is_instruction() ? c->defining_instruction() : nullptr;
            if (ka != nullptr && ka->opcode() == Opcode::iconst_i32) {
                std::swap(a, c);
                std::swap(ka, kc);
            }
            if (kc != nullptr && kc->opcode() == Opcode::iconst_i32) {
                const int32_t k = kc->imm_i32();
                Value* ovf = b.build_smul_overflow(a, c);
                if (k > 0) {
                    res_val = ovf;
                } else {
                    res_val = b.build_or(ovf, k < 0 ? b.build_eq(a, zero) : b.build_slt(a, zero));
                }
                return true;
            }
            Value* minus_zero =
                b.build_and(b.build_eq(b.build_mul(a, c), zero), b.build_slt(b.build_or(a, c), zero));
            res_val = b.build_or(b.build_smul_overflow(a, c), minus_zero);
            return true;
        }
        // bool = fn.code_is %callee, @f: an object, of the function kind,
        // whose code word is f's entry (`__wrapper_f`, what create.func and
        // the function singletons store there). Branch-free: the header and
        // code reads go through a mapped block for a non-object.
        //   p = v ^ OBJECT_TAG<<48 ; obj = p < 2^48 ; base = obj ? p : blank
        //   r = obj & ([base] == FUNCTION<<16|OBJECT) & ([base+24] == &__wrapper_f)
        case BronzeOp::FnCodeIs: {
            if (!need(1)) return false;
            if (inst.index >= resolved_names_.size() || resolved_names_[inst.index].empty()) {
                return inst_error(inst, "fn.code_is names no function of the module");
            }
            Value* ptr = object_address(b, bits(0));
            Value* is_obj = is_object_address(b, ptr);
            Value* base = prop_lowering_.object_or_blank(b, ptr, is_obj);
            Value* is_fn = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kFunctionHeaderLow));
            Value* code = b.build_load(Type::ptr(), base, kBronzeFnCodeOffset);
            Value* is_f = b.build_eq(code, b.build_func_addr("__wrapper_" + resolved_names_[inst.index]));
            res_val = b.build_and(is_obj, b.build_and(is_fn, is_f));
            return true;
        }
        // dynamic = fn.env %callee: the environment word at
        // BRONZE_ABI_FN_ENV_OFFSET.  xor ; mov rax, [rax+32]
        case BronzeOp::FnEnv: {
            if (!need(1)) return false;
            res_val = b.build_load(Type::i64(), object_address(b, bits(0)), kBronzeFnEnvOffset);
            return true;
        }
        // i32 = array.length %arr: the length word.  xor ; mov eax, [rax+24]
        case BronzeOp::ArrayLength: {
            if (!need(1)) return false;
            res_val = array_header_load(b, Type::i32(), object_address(b, bits(0)), BRONZE_ABI_ARRAY_LENGTH_OFFSET);
            return true;
        }
        // dynamic = elem.load %arr, %i: the element word as stored (a hole
        // stays the HOLE value):
        //   block = [p+40] & PAYLOAD ; head = [p+32] ; mov rax, [block + 8*(head+i) + 8]
        case BronzeOp::ElemLoad: {
            if (!need(2)) return false;
            Value* ptr = object_address(b, bits(0));
            const ArrayElements e = array_elements(b, ptr);
            if ((inst.imm_i64 & kElemLoadNumber) != 0) {
                // A NUMBERS array's element: a double, boxed as its bits.
                res_val = b.build_bitcast_i64_f64(array_element_load(b, e, b.build_zext_i64(i32(1)), Type::f64()));
                return true;
            }
            res_val = array_element_load(b, e, b.build_zext_i64(i32(1)));
            return true;
        }
        // elem.store %arr, %i, %v: the store at elem.load's address; for a
        // value that is no Number in its double form, the array's NUMBERS bit
        // cleared (bronze_abi_layout.h BRONZE_ABI_ARRAY_BIT_NUMBERS: generated
        // code that stores anything else clears it) and, for one that may be a
        // reference, the write barrier on the element's address.
        case BronzeOp::ElemStore: {
            if (!need(3)) return false;
            Value* ptr = object_address(b, bits(0));
            Value* idx = i32(1);
            Value* v = bits(2);
            const bool primitive = is_primitive_value(inst.operands[2]) || prop_lowering_.is_known_non_ref(v);
            const bool always_number = opd[2]->type() == Type::f64() || boxed_f64_source(v) != nullptr ||
                                       (inst.imm_i64 & kStoreNumber) != 0;
            auto emit_store = [&](Value* block, Value* at_idx) {
                if (always_number) {
                    prop_lowering_.store_value_indexed(b, block, at_idx, kBronzeHdrBytes, v, kBoxedElementAliasClass,
                                                       /*need_barrier=*/false);
                    return;
                }
                const auto forget_numbers = [ptr, block](Builder& bb) {
                    Value* word = array_header_load(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
                    array_header_store(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET,
                                       bb.build_and(word, bb.build_iconst_i32(~int32_t{BRONZE_ABI_ARRAY_BIT_NUMBERS})));
                    array_header_store(bb, Type::i32(), block, 0, bb.build_iconst_i32(kValueBlockHeaderLow));
                };
                prop_lowering_.store_value_indexed(b, block, at_idx, kBronzeHdrBytes, v, kBoxedElementAliasClass,
                                                   !primitive, forget_numbers);
            };
            if ((inst.imm_i64 & kStoreGrow) == 0) {
                const ArrayElements e = array_elements(b, ptr);
                emit_store(e.block, b.build_add(b.build_zext_i64(idx), e.head));
                return true;
            }
            BasicBlock* cur = b.current_block();
            const std::string prefix = "elemgrow_" + std::to_string(cur->parent()->next_block_id());
            BasicBlock* bb_append_test = b.append_block(prefix + "_aptest");
            BasicBlock* bb_append = b.append_block(prefix + "_ap");
            BasicBlock* bb_store = b.append_block(prefix + "_store");
            BasicBlock* bb_slow = b.append_block(prefix + "_slow");
            BasicBlock* bb_done = b.append_block(prefix + "_done");
            bb_slow->set_cold();

            b.position_at_end(cur);
            Value* len = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
            b.build_br_if(b.build_ult(idx, len), bb_store, bb_append_test);

            b.position_at_end(bb_append_test);
            Value* cap = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
            Value* head = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
            Value* end = b.build_add(b.build_zext_i64(head), b.build_zext_i64(idx));
            b.build_br_if(b.build_ult(end, b.build_zext_i64(cap)), bb_append, bb_slow);

            b.position_at_end(bb_append);
            array_header_store(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET,
                               b.build_add(idx, b.build_iconst_i32(1)));
            b.build_br(bb_store);

            b.position_at_end(bb_store);
            {
                const ArrayElements e = array_elements(b, ptr);
                emit_store(e.block, b.build_add(b.build_zext_i64(idx), e.head));
            }
            b.build_br(bb_done);

            b.position_at_end(bb_slow);
            b.build_call("bronze_array_push1", Type::i64(), {bits(0), v});
            b.build_br(bb_done);

            b.position_at_end(bb_done);
            return true;
        }
        // bool = in.bounds %i, %len: unsigned (cmp ; setb). Widened by a
        // span k: i >= 0 and i + k < len, in 64 bits.
        case BronzeOp::InBounds: {
            if (!need(2)) return false;
            if (inst.imm_i64 == 0) {
                res_val = b.build_ult(i32(0), i32(1));
                return true;
            }
            Value* x = b.build_sext_i64(i32(0));
            Value* last = b.build_add(x, iconst64(b, static_cast<uint64_t>(inst.imm_i64)));
            res_val = b.build_and(b.build_sge(x, iconst64(b, 0)), b.build_slt(last, b.build_sext_i64(i32(1))));
            return true;
        }
        // bool = is.hole %v: the word is BRONZE_ABI_HOLE_BITS.
        case BronzeOp::IsHole: {
            if (!need(1)) return false;
            res_val = b.build_eq(bits(0), iconst64(b, BRONZE_ABI_HOLE_BITS));
            return true;
        }
        // bool = is.uninit %v: the word is BRONZE_ABI_UNINITIALIZED_BITS.
        case BronzeOp::IsUninit: {
            if (!need(1)) return false;
            res_val = b.build_eq(bits(0), iconst64(b, BRONZE_ABI_UNINITIALIZED_BITS));
            return true;
        }
        // bool = is.array %v: an object whose header word is the Array's.
        // Branch-free, as fn.code_is: the header read goes through a mapped
        // block for a non-object.
        case BronzeOp::IsArray: {
            if (!need(1)) return false;
            Value* ptr = object_address(b, bits(0));
            Value* is_obj = is_object_address(b, ptr);
            Value* base = prop_lowering_.object_or_blank(b, ptr, is_obj);
            res_val = b.build_and(is_obj, b.build_eq(b.build_load(Type::i32(), base, 0),
                                                     b.build_iconst_i32(kArrayHeaderLow)));
            return true;
        }
        // i32 = typed.length %ta: the view header's length.  xor ; mov eax, [rax+68]
        case BronzeOp::TypedLength: {
            if (!need(1)) return false;
            res_val = with_alias_class(
                b.build_load(Type::i32(), object_address(b, bits(0)), BRONZE_ABI_TA_LENGTH_OFFSET),
                kTypedArrayHeaderAliasClass);
            return true;
        }
        // f64|i32 = typed.load %ta, %i, <kind>: the element at the view's data
        // address (its buffer's bytes plus its byte offset, il_view_stubs.cpp
        // view_data), as the kind reads (view_element_load): movss + cvtss2sd,
        // movsd, mov, or a byte load widened.
        case BronzeOp::TypedLoad: {
            if (!need(2)) return false;
            const auto kind = static_cast<int>(inst.imm_i64);
            if (kind > BRONZE_ABI_TA_KIND_FLOAT64) return inst_error(inst, "typed.load of an element kind it has no form for");
            res_val = view_element_load(b, view_data(b, object_address(b, bits(0))), b.build_zext_i64(i32(1)), kind);
            return true;
        }
        // typed.store %ta, %i, %v, <kind>: the store at typed.load's address,
        // converted as the kind takes it (view_element_store).
        case BronzeOp::TypedStore: {
            if (!need(3)) return false;
            const auto kind = static_cast<int>(inst.imm_i64);
            if (kind > BRONZE_ABI_TA_KIND_FLOAT64) return inst_error(inst, "typed.store of an element kind it has no form for");
            const bool f64 = kind == BRONZE_ABI_TA_KIND_FLOAT32 || kind == BRONZE_ABI_TA_KIND_FLOAT64 ||
                             kind == BRONZE_ABI_TA_KIND_UINT8CLAMPED;
            view_element_store(b, view_data(b, object_address(b, bits(0))), b.build_zext_i64(i32(1)), kind,
                               f64 ? ensure_type(opd[2], Type::f64(), b) : i32(2));
            return true;
        }
        // bool = fn.native_is %f, <intrinsic>: fn.code_is against the
        // runtime's exported native (`callee_name`) instead of a module
        // function's wrapper.
        case BronzeOp::FnNativeIs: {
            if (!need(1)) return false;
            if (inst.callee_name.empty()) return inst_error(inst, "fn.native_is names no intrinsic");
            Value* ptr = object_address(b, bits(0));
            Value* is_obj = is_object_address(b, ptr);
            Value* base = prop_lowering_.object_or_blank(b, ptr, is_obj);
            Value* is_fn = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kFunctionHeaderLow));
            Value* code = b.build_load(Type::ptr(), base, kBronzeFnCodeOffset);
            Value* is_f = b.build_eq(code, b.build_func_addr(inst.callee_name));
            res_val = b.build_and(is_obj, b.build_and(is_fn, is_f));
            return true;
        }
        // math.<intrinsic>: one instruction where the machine has it, else
        // the runtime's f64 kernel, the same function the native calls.
        case BronzeOp::MathFn:
            return lower_math_fn(inst, b, opd, res_val);
        default:
            return inst_error(inst, "not a speculation form");
    }
}

// The kernels below are the ones the natives themselves call
// (runtime/builtin_math.cpp), so S and G compute the same bits.
bool IlLowering::lower_math_fn(const BronzeInstruction& inst, Builder& b, const std::vector<Value*>& opd,
                               Value*& res_val) {
    // il::Intrinsic, in its order.
    enum : int64_t { kAbs, kFloor, kCeil, kRound, kSqrt, kMin, kMax, kImul, kSin, kCos, kTan, kAsin, kAcos, kAtan,
                     kAtan2, kExp, kLog };
    const size_t arity = (inst.imm_i64 == kMin || inst.imm_i64 == kMax || inst.imm_i64 == kImul ||
                          inst.imm_i64 == kAtan2) ? 2 : 1;
    if (opd.size() != arity) return inst_error(inst, "math with the wrong number of operands");
    if (inst.imm_i64 == kImul) {
        res_val = b.build_mul(ensure_type(opd[0], Type::i32(), b), ensure_type(opd[1], Type::i32(), b));
        return true;
    }
    Value* x = ensure_type(opd[0], Type::f64(), b);
    Value* y = arity == 2 ? ensure_type(opd[1], Type::f64(), b) : nullptr;
    auto kernel = [&](const char* name) {
        res_val = y != nullptr ? b.build_call(name, Type::f64(), {x, y}) : b.build_call(name, Type::f64(), {x});
        return true;
    };
    auto emit_minmax = [&](bool is_min) {
        BasicBlock* cur = b.current_block();
        const std::string p = (is_min ? "min_" : "max_") + std::to_string(cur->parent()->next_block_id());
        BasicBlock* bb_check_y = b.append_block(p + "_y");
        BasicBlock* bb_eq_or_nan = b.append_block(p + "_eqnan");
        BasicBlock* bb_eq = b.append_block(p + "_eq");
        BasicBlock* bb_nan = b.append_block(p + "_nan");
        BasicBlock* bb_merge = b.append_block(p + "_merge");
        bb_eq_or_nan->set_cold();
        bb_eq->set_cold();
        bb_nan->set_cold();
        Value* res = b.add_block_param(bb_merge, Type::f64());

        b.position_at_end(cur);
        Value* take_x = is_min ? b.build_slt(x, y) : b.build_sgt(x, y);
        b.build_br_if(take_x, bb_merge, {x}, bb_check_y, {});

        b.position_at_end(bb_check_y);
        Value* take_y = is_min ? b.build_sgt(x, y) : b.build_slt(x, y);
        b.build_br_if(take_y, bb_merge, {y}, bb_eq_or_nan, {});

        b.position_at_end(bb_eq_or_nan);
        b.build_br_if(b.build_eq(x, y), bb_eq, bb_nan);

        b.position_at_end(bb_eq);
        Value* is_neg_zero_x = b.build_eq(b.build_bitcast_i64_f64(x),
                                          iconst64(b, static_cast<int64_t>(uint64_t{1} << 63)));
        Value* eq_res = is_min ? b.build_select(is_neg_zero_x, x, y) : b.build_select(is_neg_zero_x, y, x);
        b.build_br(bb_merge, {eq_res});

        b.position_at_end(bb_nan);
        b.build_br(bb_merge, {b.build_bitcast_f64_i64(iconst64(b, BRONZE_ABI_CANONICAL_NAN_BITS))});

        b.position_at_end(bb_merge);
        res_val = res;
        return true;
    };
    switch (inst.imm_i64) {
        case kAbs: res_val = b.build_fabs_f64(x); return true;
        case kFloor: res_val = b.build_floor_f64(x); return true;
        case kCeil: res_val = b.build_ceil_f64(x); return true;
        case kSqrt: res_val = b.build_sqrt_f64(x); return true;
        case kRound: return kernel("bronze_math_round_f64");
        case kMin: return emit_minmax(/*is_min=*/true);
        case kMax: return emit_minmax(/*is_min=*/false);
        case kSin: return kernel("brass_sin_f64");
        case kCos: return kernel("brass_cos_f64");
        case kTan: return kernel("bronze_math_tan_f64");
        case kAsin: return kernel("bronze_math_asin_f64");
        case kAcos: return kernel("bronze_math_acos_f64");
        case kAtan: return kernel("bronze_math_atan_f64");
        case kAtan2: return kernel("bronze_math_atan2_f64");
        case kExp: return kernel("bronze_math_exp_f64");
        case kLog: return kernel("bronze_math_log_f64");
        default: return inst_error(inst, "math names no intrinsic");
    }
}

}  // namespace il2mir

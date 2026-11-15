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

namespace il2mir {

namespace {

constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kNegativeZeroBits = 0x8000000000000000ULL;
// |x| < 2^63 as a double's bits with the sign cleared: the doubles fptosi to
// i64 is defined on (and no NaN or infinity).
constexpr uint64_t kBelowTwoTo63Bits = 0x43E0000000000000ULL;
// A function object's header word, low half: its kind over the object tag
// (runtime/heap.h HeapObjectHeader: tag, then flags).
constexpr int32_t kFunctionHeaderLow =
    static_cast<int32_t>((uint32_t{BRONZE_ABI_OBJ_FLAGS_FUNCTION} << 16) | uint32_t{BRONZE_ABI_TAG_OBJECT});
static_assert(BRONZE_ABI_OBJ_FLAGS_OFFSET == 2, "the kind flags are the header word's second half-word");

Value* iconst64(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

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

// Element `idx` (an i32, in bounds) of the Array at `ptr`: its address.
Value* element_address(Builder& b, Value* ptr, Value* idx) {
    const ArrayElements e = array_elements(b, ptr);
    Value* at = b.build_add(b.build_zext_i64(idx), e.head);
    return b.build_add(b.build_add(e.block, b.build_shl(at, b.build_iconst_i64(3))),
                       b.build_iconst_i64(kBronzeHdrBytes));
}

}  // namespace

bool is_spec_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::LoadShape:
        case BronzeOp::ShapeCell:
        case BronzeOp::PtrEq:
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
        // bool = is.object %v:  xor rax, OBJECT_TAG<<48 ; cmp rax, 1<<48 ; setb
        case BronzeOp::IsObject: {
            if (!need(1)) return false;
            res_val = is_object_address(b, object_address(b, bits(0)));
            return true;
        }
        // dynamic = slot.load %obj, <slot>: inline slot: one load at
        // ptr + 24 + 8*slot; overflow slot: the block word at ptr + 16,
        // masked, then the load at block + 8 + 8*(slot - 62).
        case BronzeOp::SlotLoad: {
            if (!need(1)) return false;
            Value* addr = slot_address(b, object_address(b, bits(0)), inst.imm_i64);
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
        // int32 range and not -0.
        //   tag = v >> 48 == INT32 ; in = |v| < 2^63 ; f = in ? v : 0
        //   k = cvttsd2si64 f ; ok = in & (cvtsi2sd k == f) & (sext32 k == k) & v != -0
        case BronzeOp::IsI32: {
            if (!need(1)) return false;
            Value* v = bits(0);
            Value* tagged_i32 = b.build_eq(b.build_lshr(v, b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT)),
                                           b.build_iconst_i64(BRONZE_ABI_TAG_INT32));
            Value* abs_bits = b.build_and(v, iconst64(b, ~kNegativeZeroBits));
            Value* in_range = b.build_ult(abs_bits, iconst64(b, kBelowTwoTo63Bits));
            Value* f = b.build_bitcast_f64_i64(b.build_select(in_range, v, b.build_iconst_i64(0)));
            Value* k = b.build_fptosi_i64(f);
            Value* integral = b.build_eq(b.build_sitofp_f64_i64(k), f);
            Value* fits = b.build_eq(b.build_sext_i64(b.build_trunc_i32(k)), k);
            Value* not_minus_zero = b.build_ne(v, iconst64(b, kNegativeZeroBits));
            Value* double_i32 = b.build_and(b.build_and(in_range, integral), b.build_and(fits, not_minus_zero));
            res_val = b.build_or(tagged_i32, double_i32);
            return true;
        }
        // i32 = unbox.i32.unchecked %v: the Int32 box's low half, or the
        // double's truncation (exact on is.i32's true edge). The double is
        // read as 0 for an Int32 box, so the conversion never sees a NaN.
        //   tag = v >> 48 == INT32 ; f = tag ? 0 : v ; r = tag ? (i32)v : (i32)cvttsd2si64 f
        case BronzeOp::UnboxI32: {
            if (!need(1)) return false;
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
        case BronzeOp::MulWouldOvf: {
            if (!need(2)) return false;
            Value* a = i32(0);
            Value* c = i32(1);
            Value* zero = b.build_iconst_i32(0);
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
            Value* code = b.build_load(Type::i64(), base, kBronzeFnCodeOffset);
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
            Value* addr = element_address(b, ptr, i32(1));
            Value* v = bits(2);
            const bool primitive = is_primitive_value(inst.operands[2]) || prop_lowering_.is_known_non_ref(v);
            const bool always_number = opd[2]->type() == Type::f64() || boxed_f64_source(v) != nullptr;
            const auto forget_numbers = [ptr](Builder& bb) {
                Value* word = array_header_load(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
                array_header_store(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET,
                                   bb.build_and(word, bb.build_iconst_i32(~int32_t{BRONZE_ABI_ARRAY_BIT_NUMBERS})));
            };
            if (always_number) {
                prop_lowering_.store_value(b, addr, v, kBoxedElementAliasClass, /*need_barrier=*/false);
            } else {
                prop_lowering_.store_value(b, addr, v, kBoxedElementAliasClass, !primitive, forget_numbers);
            }
            return true;
        }
        // bool = in.bounds %i, %len: unsigned (cmp ; setb).
        case BronzeOp::InBounds: {
            if (!need(2)) return false;
            res_val = b.build_ult(i32(0), i32(1));
            return true;
        }
        // bool = is.hole %v: the word is BRONZE_ABI_HOLE_BITS.
        case BronzeOp::IsHole: {
            if (!need(1)) return false;
            res_val = b.build_eq(bits(0), iconst64(b, BRONZE_ABI_HOLE_BITS));
            return true;
        }
        default:
            return inst_error(inst, "not a speculation form");
    }
}

}  // namespace il2mir

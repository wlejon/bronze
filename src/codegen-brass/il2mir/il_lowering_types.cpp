// The lowering's value representations: which MIR type a Bronze type is
// computed and held in, and the conversions between them
// (IlLowering::ensure_type). Split from il_lowering.cpp.

#include "il_lowering.h"
#include "il_abi.h"
#include "il_lowering_ops.h"

#include <atomic>
#include <cmath>

namespace il2mir {

Type lower_type(BronzeType t) {
    switch (t) {
        case BronzeType::Void: return Type::void_type();
        case BronzeType::Bool: return Type::i8();
        case BronzeType::I32: return Type::i32();
        case BronzeType::F64: return Type::f64();
        case BronzeType::Str: return Type::ptr();
        case BronzeType::Dynamic: return Type::i64();
        // A raw word (a shape's address): never a root, never tagged.
        case BronzeType::Ptr: return Type::i64();
        case BronzeType::Unknown: return Type::i64();
    }
    return Type::i64();
}

Type lower_abi_type(BronzeType t) {
    if (t == BronzeType::Dynamic || t == BronzeType::Unknown) return Type::tagged();
    return lower_type(t);
}

namespace {

// Whether `inst` is a point where a collection can run: a call, or an
// explicit safepoint. Between two such points no object moves, so the bits
// of a tagged value are the same wherever in that stretch they are read.
bool may_collect(const Instruction* inst) {
    return inst->is_call() || inst->opcode() == Opcode::safepoint;
}

} // namespace

Value* IlLowering::unbox_tagged(Value* tagged, Builder& b) {
    // box(x) read back before anything could collect is x itself: the bits
    // have not had a chance to move. The scan is bounded; past it the read
    // is an ordinary unbox.
    Instruction* def = tagged->defining_instruction();
    if (def && def->opcode() == Opcode::bitcast_tagged_i64 && def->parent() == b.current_block()) {
        int budget = 256;
        for (Instruction* i = b.current_block()->tail(); i && budget-- > 0; i = i->prev()) {
            if (i == def) return def->operand(0);
            if (may_collect(i)) break;
        }
    }
    return b.build_bitcast_i64_tagged(tagged);
}

IlLowering::IlLowering(const TranslatorOptions& options, DiagnosticReporter* diag)
    : options_(options), diag_(diag), alloc_lowering_(options.enable_tlab) {
    prop_lowering_.set_inline_sets(!options.enable_census);
    prop_lowering_.set_in_process(options.in_process);
    if (options.count_slow_sites && options.slow_site_count != 0) {
        prop_lowering_.set_slow_count_fn([this](Builder& b, uint32_t index) {
            // Lowering may run on several threads.
            if (options_.slow_armed && index / 64 < options_.slow_armed->size()) {
                std::atomic_ref<uint64_t>((*options_.slow_armed)[index / 64])
                    .fetch_or(uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
            Value* at = b.build_add(module_data_addr(b, "__bronze_ic_table"),
                                    b.build_iconst_i64(ic_slow_count_offset(index, options_.slow_site_count)));
            Value* n = with_alias_class(b.build_load(Type::i64(), at, 0), kSlowCountAliasClass);
            with_alias_class(b.build_store(Type::i64(), at, 0, b.build_add(n, b.build_iconst_i64(1))),
                             kSlowCountAliasClass);
        });
    }
    if (options.cache_evidence && options.slow_site_count != 0) {
        prop_lowering_.set_shape_turn_fn([this](Builder& b, uint32_t site, Value* keyed) {
            const uint32_t index = site + kShapeTurnArm;
            if (options_.slow_armed && index / 64 < options_.slow_armed->size()) {
                std::atomic_ref<uint64_t>((*options_.slow_armed)[index / 64])
                    .fetch_or(uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
            const auto word = [&](uint32_t arm) {
                return b.build_add(module_data_addr(b, "__bronze_ic_table"),
                                   b.build_iconst_i64(ic_slow_count_offset(site + arm, options_.slow_site_count)));
            };
            Value* last_at = word(kLastShapeArm);
            Value* turns_at = word(kShapeTurnArm);
            Value* now = b.build_or(keyed, b.build_iconst_i64(INT64_MIN));
            Value* last = with_alias_class(b.build_load(Type::i64(), last_at, 0), kSlowCountAliasClass);
            Value* n = with_alias_class(b.build_load(Type::i64(), turns_at, 0), kSlowCountAliasClass);
            with_alias_class(b.build_store(Type::i64(), turns_at, 0, b.build_add(n, b.build_zext_i64(b.build_ne(now, last)))),
                             kSlowCountAliasClass);
            with_alias_class(b.build_store(Type::i64(), last_at, 0, now), kSlowCountAliasClass);
        });
    }
    // ARMS (il_property.h arm_begin). An arm is on the instruction's fast path, so
    // its mark is one store of a constant where a slow arm counts; a counted
    // arm (arm_count) adds one.
    if (options.count_slow_sites && options.slow_site_count != 0) {
        prop_lowering_.set_arm_mark_fn([this](Builder& b, uint32_t index, bool count) {
            if (options_.slow_armed && index / 64 < options_.slow_armed->size()) {
                std::atomic_ref<uint64_t>((*options_.slow_armed)[index / 64])
                    .fetch_or(uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
            Value* at = b.build_add(module_data_addr(b, "__bronze_ic_table"),
                                    b.build_iconst_i64(ic_slow_count_offset(index, options_.slow_site_count)));
            Value* now = b.build_iconst_i64(1);
            if (count) {
                now = b.build_add(with_alias_class(b.build_load(Type::i64(), at, 0), kSlowCountAliasClass), now);
            }
            with_alias_class(b.build_store(Type::i64(), at, 0, now), kSlowCountAliasClass);
        });
    }
}

Value* IlLowering::ensure_type(Value* val, Type target_type, Builder& b) {
    if (!val || val->type() == target_type) return val;

    // A tagged value is the rooted form of a Value's bits: boxing takes the
    // bits as an i64, and box(unbox(x)) is x (x is the one of the two a
    // collection keeps current). Every look-through here returns a value of
    // the current block's making only: one from an earlier block may be from
    // before a resume point, where the twin does not have it.
    if (target_type.is_tagged()) {
        Value* bits = ensure_type(val, Type::i64(), b);
        Instruction* def = bits->defining_instruction();
        if (def && def->opcode() == Opcode::bitcast_i64_tagged && def->parent() == b.current_block()) {
            return def->operand(0);
        }
        return b.build_bitcast_tagged_i64(bits);
    }
    if (val->type().is_tagged()) {
        return ensure_type(unbox_tagged(val, b), target_type, b);
    }

    Type src_type = val->type();
    if (target_type == Type::i8()) {
        if (src_type == Type::i64()) {
            val = b.build_trunc_i32(val);
        }
        return b.build_trunc_i8(val);
    }
    if (src_type == Type::i8()) {
        val = b.build_zext_i64(val);
        if (target_type == Type::i32()) {
            return b.build_trunc_i32(val);
        }
        src_type = Type::i64();
    }
    if (src_type == Type::f64() && target_type == Type::i32()) {
        // The int32 a double was made from is that int32 (made in this
        // block: a value from before a resume point is not in its state,
        // il_lowering_resume.cpp).
        if (const Instruction* def = val->defining_instruction()) {
            if (def->opcode() == Opcode::sitofp_f64_i32 && def->parent() == b.current_block()) {
                return def->operand(0);
            }
            if (def->opcode() == Opcode::fconst_f64 && def->imm_f64() >= -2147483648.0 &&
                def->imm_f64() <= 2147483647.0 && def->imm_f64() == std::trunc(def->imm_f64())) {
                return b.build_iconst_i32(static_cast<int32_t>(def->imm_f64()));
            }
        }
        return b.build_fptosi_i32(val);
    }
    if (src_type == Type::i32() && target_type == Type::f64()) {
        return b.build_sitofp_f64_i32(val);
    }
    if (src_type == Type::i32() && target_type == Type::i64()) {
        return b.build_sext_i64(val);
    }
    if (src_type == Type::i64() && target_type == Type::i32()) {
        return b.build_trunc_i32(val);
    }
    if (src_type == Type::f64() && target_type == Type::i64()) {
        if (val->defining_instruction() &&
            val->defining_instruction()->opcode() == Opcode::bitcast_f64_i64 &&
            val->defining_instruction()->parent() == b.current_block() &&
            val->defining_instruction()->operand(0)->type() == Type::i64()) {
            return val->defining_instruction()->operand(0);
        }
        return box_f64_bits(b, val);
    }
    if (src_type == Type::i64() && target_type == Type::f64()) {
        return b.build_bitcast_f64_i64(val);
    }
    return val;
}

} // namespace il2mir

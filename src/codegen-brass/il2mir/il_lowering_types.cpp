// The lowering's value representations: which MIR type a Bronze type is
// computed and held in, and the conversions between them
// (IlLowering::ensure_type). Split from il_lowering.cpp.

#include "il_lowering.h"
#include "il_lowering_ops.h"

namespace il2mir {

Type lower_type(BronzeType t) {
    switch (t) {
        case BronzeType::Void: return Type::void_type();
        case BronzeType::Bool: return Type::i8();
        case BronzeType::I32: return Type::i32();
        case BronzeType::F64: return Type::f64();
        case BronzeType::Str: return Type::ptr();
        case BronzeType::Dynamic: return Type::i64();
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
    : options_(options), diag_(diag), prop_lowering_(options.enable_inlined_fastpaths),
      alloc_lowering_(options.enable_tlab) {
    spec_.set_feedback(options.spec_feedback.get());
    // An AOT lowering with a profile lays its sites out as the tiered
    // lowering does (every keyed site inline), minus what only an in-process
    // module can hold (a host address in the code).
    if (!options.spec_feedback && options.spec_profile) spec_.set_profile(options.spec_profile.get());
    prop_lowering_.set_spec(&spec_);
    prop_lowering_.set_inline_sets(!options.enable_census);
    prop_lowering_.set_feedback_driven(options.spec_feedback != nullptr || spec_.has_profile());
    prop_lowering_.set_in_process(options.spec_feedback != nullptr);
    prop_lowering_.set_key_names(&options_.key_constants);
    // Only the table address is shared: it is live across the body anyway
    // (it replaces the module delta every site read), where a shared key map
    // address or guard constant would join every guard's state for nothing.
    prop_lowering_.set_table_base_fn([this](Builder& b) { return hoisted(b, Hoist::IcTable); });
    // BRONZE_IC_FULL=1: every inline path in full everywhere, the lowering
    // before the small forms and run-once code (the A/B seam).
    static const bool full = [] {
        const char* v = std::getenv("BRONZE_IC_FULL");
        return v && v[0] == '1';
    }();
    small_forms_ = !full;
    prop_lowering_.set_small_forms(small_forms_);
}

Value* IlLowering::ensure_type(Value* val, Type target_type, Builder& b) {
    if (!val || val->type() == target_type) return val;

    // A tagged value is the rooted form of a Value's bits: boxing takes the
    // bits as an i64, and box(unbox(x)) is x (x is the one of the two a
    // collection keeps current).
    if (target_type.is_tagged()) {
        Value* bits = ensure_type(val, Type::i64(), b);
        Instruction* def = bits->defining_instruction();
        if (def && def->opcode() == Opcode::bitcast_i64_tagged) return def->operand(0);
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

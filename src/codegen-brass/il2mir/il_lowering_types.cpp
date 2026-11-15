// The lowering's value representations: which MIR type a Bronze type is
// computed and held in, and the conversions between them
// (IlLowering::ensure_type). Split from il_lowering.cpp.

#include "il_lowering.h"
#include "il_abi.h"
#include "il_lowering_ops.h"

#include <atomic>
#include <cmath>
#include <unordered_map>

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
    spec_.set_image_sites(options.twin_exits);
    // An AOT lowering with a profile lays its sites out as the tiered
    // lowering does (every keyed site inline), minus what only an in-process
    // module can hold (a host address in the code).
    if (!options.spec_feedback && options.spec_profile) spec_.set_profile(options.spec_profile.get());
    prop_lowering_.set_spec(&spec_);
    prop_lowering_.set_inline_sets(!options.enable_census);
    prop_lowering_.set_feedback_driven(options.spec_feedback != nullptr || spec_.has_profile());
    prop_lowering_.set_in_process(options.spec_feedback != nullptr);
    prop_lowering_.set_key_names(&options_.key_constants);
    prop_lowering_.set_layout_cells(&options_.layout_cells);
    prop_lowering_.set_class_family_objects(&options_.class_family_object_slots);
    // The table addresses are shared, computed once at entry: the IC table,
    // and the static-slot sites' cell table and family-range table (each
    // site otherwise computed its realm-relative address afresh). A shared
    // key map address or guard constant would join every guard's state for
    // nothing.
    prop_lowering_.set_table_base_fn([this](Builder& b) { return hoisted(b, Hoist::IcTable); });
    // The guard tables stand at fixed distances after the IC table
    // (il_abi.h ic_static_cells_offset), so they are that base plus a
    // constant where they are used: one value held across a function's calls.
    prop_lowering_.set_static_base_fn([this](Builder& b, bool family) {
        const uint64_t sites = options_.ic_site_count;
        const uint64_t methods = options_.method_ic_sites.size();
        const int64_t off = family ? ic_family_base_offset(sites, methods, options_.static_site_count)
                                   : ic_static_cells_offset(sites, methods);
        return b.build_add(hoisted(b, Hoist::IcTable), b.build_iconst_i64(off));
    });
    if (options.count_slow_sites && options.slow_site_count != 0) {
        prop_lowering_.set_slow_count_fn([this](Builder& b, uint32_t index) {
            // Lowering may run on several threads.
            if (options_.slow_armed && index / 64 < options_.slow_armed->size()) {
                std::atomic_ref<uint64_t>((*options_.slow_armed)[index / 64])
                    .fetch_or(uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
            Value* at = b.build_add(hoisted(b, Hoist::IcTable),
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
                return b.build_add(hoisted(b, Hoist::IcTable),
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
    // ARMS (il_speculation.h). An arm is on the instruction's fast path, so
    // its mark is one store of a constant where a slow arm counts.
    std::function<void(Builder&, uint32_t)> mark;
    if (options.count_slow_sites && options.slow_site_count != 0) {
        mark = [this](Builder& b, uint32_t index) {
            if (options_.slow_armed && index / 64 < options_.slow_armed->size()) {
                std::atomic_ref<uint64_t>((*options_.slow_armed)[index / 64])
                    .fetch_or(uint64_t{1} << (index % 64), std::memory_order_relaxed);
            }
            Value* at = b.build_add(hoisted(b, Hoist::IcTable),
                                    b.build_iconst_i64(ic_slow_count_offset(index, options_.slow_site_count)));
            with_alias_class(b.build_store(Type::i64(), at, 0, b.build_iconst_i64(1)), kSlowCountAliasClass);
        };
    }
    if (!options.count_slow_sites && options.slow_site_count != 0) {
        prop_lowering_.set_arm_cell_fn([this](Builder& b, uint32_t index) {
            return b.build_add(hoisted(b, Hoist::IcTable),
                               b.build_iconst_i64(ic_slow_count_offset(index, options_.slow_site_count)));
        });
    }
    prop_lowering_.set_arm_fns(
        [this](uint32_t index) {
            return options_.twin_exits && spec_.image_sites_here() && options_.slow_cold.count(index) != 0;
        },
        std::move(mark));
}

// The f64 block parameters of `fn` that only ever hold an int32: every value
// an edge passes is the result of a bitwise operation or Math.imul (each an
// int32 by definition), a constant that is one, or another such parameter.
// The lowering carries these as i32 (IlLowering's block-parameter setup), so
// a value a loop keeps in the integer domain (`h = Math.imul(h ^ x, k)`, a
// bit set, a PRNG's state) crosses the back edge as the integer it is and not
// as a double converted back at every use.
//
// Found optimistically: every f64 parameter of a block reached only by jumps
// and branches starts in the set, and one leaves when an edge passes it a
// value that is none of the above, until nothing changes. `>>>` is a uint32
// and -0 is no int32, so neither qualifies.
std::unordered_set<uint32_t> int32_block_params(const BronzeFunction& fn) {
    std::unordered_map<uint32_t, const BronzeInstruction*> def;
    std::unordered_set<uint32_t> handlers;
    std::unordered_map<uint32_t, const BronzeBlock*> block_of;
    for (const BronzeBlock& blk : fn.blocks) {
        block_of.emplace(blk.id, &blk);
        if (blk.handler_id != UINT32_MAX) handlers.insert(blk.handler_id);
        for (const BronzeInstruction& inst : blk.instructions) {
            if (inst.result_id != UINT32_MAX) def.emplace(inst.result_id, &inst);
        }
    }
    std::unordered_set<uint32_t> set;
    for (size_t i = 1; i < fn.blocks.size(); ++i) {
        const BronzeBlock& blk = fn.blocks[i];
        if (handlers.count(blk.id)) continue;
        for (const auto& p : blk.params) {
            if (p.second == BronzeType::F64) set.insert(p.first);
        }
    }
    if (set.empty()) return set;
    const auto is_int32 = [&](uint32_t id) {
        if (set.count(id)) return true;
        const auto it = def.find(id);
        if (it == def.end()) return false;
        const BronzeInstruction& d = *it->second;
        switch (d.op) {
            case BronzeOp::BitAnd:
            case BronzeOp::BitOr:
            case BronzeOp::BitXor:
            case BronzeOp::Shl:
            case BronzeOp::Shr:
            case BronzeOp::BitNot:
            case BronzeOp::MathImul:
                return d.result_type == BronzeType::F64;
            case BronzeOp::ConstF64:
                return d.imm_f64 >= -2147483648.0 && d.imm_f64 <= 2147483647.0 &&
                       d.imm_f64 == std::trunc(d.imm_f64) && !std::signbit(d.imm_f64);
            default:
                return false;
        }
    };
    for (bool changed = true; changed;) {
        changed = false;
        const auto check = [&](const BronzeBlockTarget& tgt) {
            const auto it = block_of.find(tgt.block_id);
            if (it == block_of.end()) return;
            const auto& params = it->second->params;
            for (size_t i = 0; i < params.size(); ++i) {
                if (!set.count(params[i].first)) continue;
                if (i < tgt.args.size() && is_int32(tgt.args[i])) continue;
                set.erase(params[i].first);
                changed = true;
            }
        };
        for (const BronzeBlock& blk : fn.blocks) {
            for (const BronzeInstruction& inst : blk.instructions) {
                if (inst.op == BronzeOp::Jump) check(inst.target);
                if (inst.op == BronzeOp::Branch) {
                    check(inst.target);
                    check(inst.else_target);
                }
            }
        }
    }
    return set;
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
        // The int32 a double was made from is that int32.
        if (const Instruction* def = val->defining_instruction()) {
            if (def->opcode() == Opcode::sitofp_f64_i32) return def->operand(0);
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

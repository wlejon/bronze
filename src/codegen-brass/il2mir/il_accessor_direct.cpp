// The direct accessor edge (il_accessor_direct.h). A site such as
// `rotation.x += d` on a three.js `Euler`, whose `x` is `get x()` / `set x(v)`
// on the class, otherwise reaches the getter through the read stub: its four
// ways compared, the entry's depth decoded, the chain walked to the holder,
// the getter loaded and tested, and an indirect call through the getter's
// wrapper, which unpacks an argument vector the stub had to build. Here the
// hit is one guard on the site's accessor cell and an ordinary direct call of
// the getter's body, which the guarded inliner can then copy in.
//
// The guard is the cell's and not the site's cache, because the cache cannot
// answer the question the direct call needs answered: an accessor entry names
// a holder and a slot, not which function is in it. The cell names exactly
// that, for one shape and one epoch (runtime/accessor_cell.cpp), and its fill
// runs on a miss only while the cell speaks for an older epoch, so a site the
// guess is wrong for — an own `x`, a subclass's override, a patched prototype
// — pays one call per epoch and otherwise keeps the cache it always had.

#include "il_accessor_direct.h"

#include "il_abi.h"
#include "il_lowering.h"
#include "il_lowering_direct.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr const char* kMissName = "_miss_cold";

Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

std::string block_prefix(Builder& b, const char* what) {
    return std::string(what) + "_" + std::to_string(b.current_block()->parent()->next_block_id());
}

// The accessor the site names, as the module function its hit calls: the
// getter takes `this`, the setter `this` and the value, both boxed. The
// resolver stamped only bodies of that shape (direct_method_table.cpp), so a
// site whose target is missing or shaped otherwise is a lowering bug, refused
// by name rather than lowered without its call.
Function* accessor_target(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, bool setter) {
    const std::string name = lowering->resolve_callee(inst.callee_name);
    Function* fn = b.current_block()->parent()->parent()->get_function(name);
    const auto it = lowering->options().function_meta.find(name);
    const size_t want = setter ? 2 : 1;
    if (fn && fn->param_types().size() == want && it != lowering->options().function_meta.end() &&
        it->second.needs_this && !it->second.needs_env && !it->second.needs_arguments &&
        !it->second.has_rest_param) {
        return fn;
    }
    lowering->set_has_error(true);
    if (lowering->diag()) {
        lowering->diag()->error(SourceLocation("", inst.line, inst.column),
                                "il2mir: accessor site names '" + inst.callee_name +
                                    "', which is no direct-callable " + (setter ? "setter" : "getter"));
    }
    return nullptr;
}

// The fill after a miss's ordinary access: the publish, only while the cell
// speaks for some other epoch (runtime/accessor_cell.cpp). `obj` must be bits
// read after the access, which may have collected. Leaves the builder after it.
void emit_accessor_fill(IlLowering* lowering, Builder& b, Value* obj, const BronzeInstruction& inst,
                        const std::string& callee, bool setter) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    BasicBlock* cur = b.current_block();
    const std::string prefix = block_prefix(b, "accessor_fill");
    // Named cold, which brass's block layout reads (codegen/block_layout.cpp):
    // a fill runs once per site and epoch, and is no part of any trace.
    BasicBlock* call = b.append_block(prefix + "_call_cold");
    BasicBlock* done = b.append_block(prefix + "_done");
    b.position_at_end(cur);
    Value* cells = pl.static_cells_addr(b);
    const int32_t off = static_cast<int32_t>(inst.static_cell * sizeof(uint64_t));
    Value* epoch = b.build_load(Type::i64(), tls_block(b), BRONZE_TLS_PROTO_EPOCH_OFF);
    Value* stale = b.build_ne(b.build_load(Type::i64(), cells, off + 8), epoch);
    b.build_br_if(stale, call, done);
    b.position_at_end(call);
    b.build_call("bronze_accessor_cell_publish", Type::void_type(),
                 {obj, lowering->get_key_id(b, inst.index), b.build_add(cells, b.build_iconst_i64(off)),
                  b.build_func_addr("__wrapper_" + callee), b.build_iconst_i32(setter ? 1 : 0)});
    b.build_br(done);
    call->set_cold();
    b.position_at_end(done);
}

}  // namespace

bool accessor_direct_here(IlLowering* lowering, const BronzeInstruction& inst) {
    if (!lowering || inst.callee_name.empty() || inst.operands.empty()) return false;
    if (!direct_method_ic_here(lowering) || lowering->run_once()) return false;
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    if (!pl.enable_inlined_fastpaths() || inst.index == PropertyLoweringHelper::kNoKey) return false;
    if (!pl.static_cells_cover(inst.static_cell, 2)) {
        lowering->set_has_error(true);
        if (lowering->diag()) {
            lowering->diag()->error(SourceLocation("", inst.line, inst.column),
                                    "il2mir: accessor cell " + std::to_string(inst.static_cell) + " of '" +
                                        std::string(pl.key_tag(inst.index)) +
                                        "' is past the module's static cells");
        }
        return false;
    }
    return true;
}

Value* accessor_cell_hit(IlLowering* lowering, Builder& b, Value* recv, uint32_t cell, Value* keyed) {
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    Value* cells = pl.static_cells_addr(b);
    const int32_t off = static_cast<int32_t>(cell * sizeof(uint64_t));
    Value* want = b.build_load(Type::i64(), cells, off);
    Value* at = b.build_load(Type::i64(), cells, off + 8);
    Value* epoch = b.build_load(Type::i64(), tls_block(b), BRONZE_TLS_PROTO_EPOCH_OFF);
    Value* same_epoch = b.build_eq(at, epoch);
    if (keyed) {
        // The receiver's keyed shape (the shape memo's word): a plain
        // object's shape, else kNotPlainShape (2). A cell's first word is 0,
        // kRefused (1) or an 8-aligned shape (runtime/accessor_cell.cpp), so
        // `keyed == cell[0]` already says "plain and that shape".
        return b.build_and(b.build_eq(keyed, want), same_epoch);
    }
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    // A non-object reads a mapped block whose shape word no cell holds.
    pl.receiver_shape(b, recv, plain, ptr, shape);
    return b.build_and(plain, b.build_and(b.build_eq(shape, want), same_epoch));
}

Value* lower_accessor_get(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* obj,
                          const AccessorMiss& miss) {
    Function* target = accessor_target(lowering, inst, b, /*setter=*/false);
    if (!target) return b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag));
    const std::string callee = lowering->resolve_callee(inst.callee_name);
    BasicBlock* cur = b.current_block();
    const std::string prefix = block_prefix(b, "accessor_get");
    BasicBlock* hit = b.append_block(prefix + "_hit");
    BasicBlock* missed = b.append_block(prefix + kMissName);
    BasicBlock* merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(merge, Type::i64());

    b.position_at_end(cur);
    b.build_br_if(accessor_cell_hit(lowering, b, obj, inst.static_cell), hit, missed);

    // The call [[Get]] makes (10.1.8.1 step 7: Call(getter, Receiver)), with
    // the receiver as `this` and no arguments.
    b.position_at_end(hit);
    Value* got = b.build_call(callee, target->return_type(), {lowering->ensure_type(obj, target->param_types()[0], b)});
    Value* boxed = target->return_type() == Type::void_type() ? b.build_iconst_i64(static_cast<int64_t>(kUndefinedTag))
                                                              : lowering->ensure_type(got, Type::i64(), b);
    b.build_br(merge, {boxed});

    b.position_at_end(missed);
    Value* slow = miss.get(b);
    emit_accessor_fill(lowering, b, miss.receiver(b), inst, callee, /*setter=*/false);
    b.build_br(merge, {slow});

    b.position_at_end(merge);
    return result;
}

void lower_accessor_set(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* obj, Value* val,
                        const AccessorMiss& miss) {
    Function* target = accessor_target(lowering, inst, b, /*setter=*/true);
    if (!target) return;
    const std::string callee = lowering->resolve_callee(inst.callee_name);
    BasicBlock* cur = b.current_block();
    const std::string prefix = block_prefix(b, "accessor_set");
    BasicBlock* hit = b.append_block(prefix + "_hit");
    BasicBlock* missed = b.append_block(prefix + kMissName);
    BasicBlock* merge = b.append_block(prefix + "_merge");

    b.position_at_end(cur);
    b.build_br_if(accessor_cell_hit(lowering, b, obj, inst.static_cell), hit, missed);

    // The call [[Set]] makes (10.1.9.2 step 7: Call(setter, Receiver, «V»));
    // its result is discarded, and the assignment's value is the operand.
    b.position_at_end(hit);
    const auto& params = target->param_types();
    b.build_call(callee, target->return_type(),
                 {lowering->ensure_type(obj, params[0], b), lowering->ensure_type(val, params[1], b)});
    b.build_br(merge);

    b.position_at_end(missed);
    miss.set(b);
    emit_accessor_fill(lowering, b, miss.receiver(b), inst, callee, /*setter=*/true);
    b.build_br(merge);

    b.position_at_end(merge);
}

}  // namespace il2mir

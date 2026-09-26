#include "il_lowering_coro.h"
#include "il_lowering.h"
#include "il_abi.h"
#include <brass/mir/module.hpp>

namespace il2mir {

bool is_coro_il_op(BronzeOp op) {
    switch (op) {
        case BronzeOp::CoroStart:
        case BronzeOp::CoroSuspend:
        case BronzeOp::CoroMode:
        case BronzeOp::IterOpen:
        case BronzeOp::IterStep:
        case BronzeOp::IterValue:
        case BronzeOp::IterClose:
        case BronzeOp::IterRest:
        case BronzeOp::IterDelegate:
        case BronzeOp::AsyncIterOpen:
        case BronzeOp::AsyncIterNext:
        case BronzeOp::AsyncIterClose:
            return true;
        default:
            return false;
    }
}

bool lower_coro_instruction(
    IlLowering* lowering,
    const BronzeInstruction& inst_ast,
    Builder& b,
    Function* /*fn*/,
    std::unordered_map<uint32_t, Value*>& val_map,
    Value*& res_val
) {
    auto get_opd = [&](size_t idx) -> Value* {
        if (idx < inst_ast.operands.size()) {
            uint32_t oid = inst_ast.operands[idx];
            if (lowering) {
                return lowering->get_val_by_id(oid, b, val_map);
            }
            if (val_map.count(oid)) return val_map[oid];
        }
        return nullptr;
    };

    switch (inst_ast.op) {
        case BronzeOp::CoroStart: {
            // A fresh frame of the body over the call's own arguments, handed
            // to the runtime, which runs it to its first suspension and
            // answers the generator object or the promise.
            // A dynamic argument goes in as its tagged value, not the bits
            // get_opd unboxes: the frame's allocation may collect, and the
            // stores into the frame come after it, so the argument has to
            // be a root across the create for the collector to update it.
            std::vector<Value*> args;
            for (size_t i = 0; i < inst_ast.operands.size(); ++i) {
                auto it = val_map.find(inst_ast.operands[i]);
                if (it != val_map.end() && it->second && it->second->type().is_tagged()) {
                    args.push_back(it->second);
                } else {
                    args.push_back(get_opd(i));
                }
            }
            const std::string callee = lowering->resolve_callee(inst_ast.callee_name);
            Value* frame = b.build_coro_create(callee, Span<Value* const>(args.data(), args.size()));
            Value* kind = b.build_iconst_i32(static_cast<int32_t>(inst_ast.imm_i64));
            res_val = b.build_call("bronze_coro_start", Type::i64(), {kind, frame});
            return true;
        }

        case BronzeOp::CoroSuspend: {
            const auto kind = static_cast<uint32_t>(inst_ast.imm_i64);
            // The suspend returns the value from the body, whose return type
            // is tagged.
            auto it = val_map.find(inst_ast.operands.empty() ? UINT32_MAX : inst_ast.operands[0]);
            Value* out = (it != val_map.end() && it->second && it->second->type().is_tagged())
                             ? it->second
                             : lowering->ensure_type(get_opd(0), Type::tagged(), b);
            Value* cell = b.build_coro_suspend(out, lowering->next_coro_state_id(kind), Type::i64());
            if (lowering->options().pin_tls_register) {
                // Resumed from C++ (brass's resume), which does not carry
                // the pinned register.
                Value* tls = b.build_call("bronze_tls_block_addr", Type::i64(), {});
                b.build_pinned_tls_write(tls);
            }
            // The resumer passes the address of a rooted cell holding the
            // sent value (bronze_abi.h, COROUTINE BODIES).
            res_val = b.build_load(Type::i64(), cell, 0);
            return true;
        }

        case BronzeOp::CoroMode: {
            res_val = b.build_coro_resume_mode(lowering->coro_frame());
            return true;
        }

        case BronzeOp::IterOpen: {
            Value* gen = get_opd(0);
            res_val = b.build_call("bronze_iter_open", Type::i64(), {gen});
            return true;
        }

        case BronzeOp::IterStep: {
            Value* iter = get_opd(0);
            if (inst_ast.result_type == BronzeType::Bool) {
                res_val = b.build_and(b.build_call("bronze_iter_step", Type::i32(), {iter}), b.build_iconst_i32(1));
            } else {
                res_val = b.build_call("bronze_iter_step", Type::i64(), {iter});
            }
            return true;
        }

        case BronzeOp::IterValue: {
            Value* iter = get_opd(0);
            res_val = b.build_call("bronze_iter_value", Type::i64(), {iter});
            return true;
        }

        case BronzeOp::IterClose: {
            Value* iter = get_opd(0);
            Value* suppress = b.build_iconst_i32(inst_ast.imm_i64 != 0 ? 1 : 0);
            b.build_call("bronze_iter_close", Type::void_type(), {iter, suppress});
            return true;
        }

        case BronzeOp::IterRest: {
            Value* iter = get_opd(0);
            res_val = b.build_call("bronze_iter_rest", Type::i64(), {iter});
            return true;
        }

        case BronzeOp::IterDelegate: {
            Value* iter = get_opd(0);
            Value* mode = get_opd(1);
            Value* sent = get_opd(2);
            res_val = b.build_call("bronze_iter_delegate", Type::i64(), {iter, mode, sent});
            return true;
        }

        case BronzeOp::AsyncIterOpen: {
            Value* iter = get_opd(0);
            res_val = b.build_call("bronze_async_iter_open", Type::i64(), {iter});
            return true;
        }

        case BronzeOp::AsyncIterNext: {
            Value* iter = get_opd(0);
            res_val = b.build_call("bronze_async_iter_next", Type::i64(), {iter});
            return true;
        }

        case BronzeOp::AsyncIterClose: {
            Value* iter = get_opd(0);
            Value* suppress = b.build_iconst_i32(inst_ast.imm_i64 != 0 ? 1 : 0);
            b.build_call("bronze_async_iter_close", Type::void_type(), {iter, suppress});
            return true;
        }

        default:
            return false;
    }
}

} // namespace il2mir

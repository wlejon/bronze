#pragma once

// The baseline lowering of calls (il_call_entry.cpp): every call, method call
// and `new` tests its callee inline and enters compiled code itself when the
// function object says it may (bronze_abi.h, BRONZE_ABI_FN_FLAG_DIRECT_CALL),
// with the runtime helper behind every test.

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>

#include <string>
#include <vector>

namespace il2mir {

class IlLowering;

// The module function a protected block enters code through
// (build_enter_code_trampoline): brass has no invoke of a register, so a
// call in a block with a handler is a direct call of this, which becomes the
// invoke route_exception_edges makes of every call there.
constexpr const char* kEnterCodeTrampoline = "__bronze_enter_code";
void build_enter_code_trampoline(Function* fn);

// `callee(this, ...args)`.
Value* lower_call_dynamic_inline(IlLowering* lowering, Builder& b, Value* callee, Value* this_val,
                                 const std::vector<Value*>& args);

// `recv.key(...args)` through method site `site` (its words at
// site.base + site.off), the helper bronze_call_method behind it. `key_index`
// is the key's index into the module's key constants.
Value* lower_method_call_inline(IlLowering* lowering, Builder& b, Value* recv, Value* key_id, uint32_t key_index,
                                const std::vector<Value*>& args, uint32_t ic_index);

// `recv.apply(this_val, list)`, the list the first `count` (a boxed Number)
// of `args` (il_op.h apply.args), through method site `ic_index`.
Value* lower_apply_args_inline(IlLowering* lowering, Builder& b, Value* recv, Value* this_val, Value* count,
                               Value* key_id, const std::vector<Value*>& args, uint32_t ic_index);

// `super(...args)` with the active function `active` and the receiver
// `this_val` (13.3.7.1): the base entered directly when it may be.
Value* lower_super_call_inline(IlLowering* lowering, Builder& b, Value* active, Value* this_val,
                               const std::vector<Value*>& args);

// A callee compiled code may call without the runtime (the call form of the
// flags-word test, a native's arity checked against the i32 `argc`): leaves
// the builder where it may and returns the function's payload; any other value
// branches to `miss`.
Value* direct_callee(Builder& b, Value* callee, Value* argc, BasicBlock* miss);
// The function at payload `fn` entered with `this_val` and `argc` arguments at
// `argv`, from a block with no handler (a module stub's).
Value* call_function_code(Builder& b, Value* fn, Value* this_val, Value* argc, Value* argv);

// `new ctor(...args)`.
// `slow_site`: the construct's, an allocation site (pretenured, or counted
// with a memento: bump_plain_object).
Value* lower_construct_inline(IlLowering* lowering, Builder& b, Value* ctor, const std::vector<Value*>& args,
                              uint32_t slow_site);

}  // namespace il2mir

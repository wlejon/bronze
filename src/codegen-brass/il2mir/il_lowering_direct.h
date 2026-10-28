#pragma once

// Calls that enter a compiled callee's code from the call site itself, with
// the runtime's helper as the slow path (bronze_abi.h,
// BRONZE_ABI_FN_FLAG_DIRECT_CALL says what that skips and why it may).

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <functional>
#include <vector>

namespace il2mir {

class IlLowering;

// Whether the site being lowered takes the direct forms below: the module is
// an image build (TranslatorOptions::direct_js_calls), the block runs more
// than once, and it has no handler (an indirect call cannot be an invoke).
bool direct_entry_here(const IlLowering* lowering);
// Whether a method call takes lower_method_call_ic: an image build, in any
// block — its dispatch is a direct call, which a handler's block may invoke.
bool direct_method_ic_here(const IlLowering* lowering);

// `callee(...args)` with `this_val`: enters the callee's code when it is a
// function object compiled code may enter here, else bronze_dynamic_call.
// `boxed_args` are Value bits; they are staged in the argv block first, which
// both paths read.
Value* lower_closure_call(IlLowering* lowering, Builder& b, Value* callee, Value* this_val,
                          const std::vector<Value*>& boxed_args);

// Builds the module's stubs that the forms below call (once, before any body).
void emit_direct_stubs(IlLowering* lowering, Module& mod);

// `recv.key(...args)` through the site's method IC (bronze_abi.h, the
// METHOD-CALL site contract): a DIRECT entry of either way, a SLOT entry whose
// slot holds an enterable function, and an Array's EXOTIC entry, each when
// its latch recorded BRONZE_ABI_METHOD_IC_DIRECT_ENTRY_BIT; anything else is
// bronze_call_method, which reads the same words. The dispatch is the
// module's one stub function (il_lowering_direct.cpp), which the site calls.
Value* lower_method_call_ic(IlLowering* lowering, Builder& b, Value* recv, Value* key_id,
                            const std::vector<Value*>& boxed_args, Value* site);

// `recv.key(...args)` at a site whose key a receiver may hold in an own slot
// (il.h `kMethodOwnCallee`): the method cache's SLOT form dispatched here —
// way 0 names the receiver's shape with a slot, and the function in that slot
// may be entered directly — and everything else `rest`, the site's ordinary
// dispatch. `boxed_args` are staged for the slot call as `rest` stages them.
Value* lower_method_slot_inline(IlLowering* lowering, Builder& b, Value* recv, const std::vector<Value*>& boxed_args,
                                Value* site, const std::function<Value*(Builder&)>& rest);

// `new ctor(...args)`: a vetted constructor (bronze_construct has taken its
// plain path once) that compiled code may enter gets its instance bumped
// inline and its code entered with it; the result is the constructor's
// object if it returned one (10.2.2 step 10 via OrdinaryCallEvaluateBody),
// else the instance. Anything else is bronze_construct.
Value* lower_construct_direct(IlLowering* lowering, Builder& b, Value* ctor, const std::vector<Value*>& boxed_args);

}  // namespace il2mir

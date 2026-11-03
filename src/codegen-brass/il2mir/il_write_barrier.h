#pragma once

// The write barrier after a store generated code makes itself.

#include <brass/mir/builder.hpp>

namespace il2mir {

using brass::Builder;
using brass::Value;

// brass_gc_write_barrier(obj, val), behind the one test that answers most
// stores without the call: a target in this thread's young generation
// (bronze_abi_tls.h, young_base / young_span). `obj` is the object stored
// into, as a Value or any address inside it, exactly the barrier's operand;
// `val` is the Value stored. The caller has already decided `val` may be a
// reference (it is above the Number range, or not known).
//
// In a module without the pinned TLS register the block is itself a call
// away, so the barrier is called directly there.
void build_write_barrier(Builder& b, Value* obj, Value* val);

}  // namespace il2mir

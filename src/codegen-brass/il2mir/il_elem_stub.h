#pragma once

// The module's keyed-access stubs (il_elem_stub.cpp): bronze_elem_get's and
// bronze_elem_set's operands and answers, with the computed-access caches'
// plain-object hits inline.

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

class IlLowering;

inline constexpr const char* kElemGetStub = "__bronze_elem_get_keyed";
inline constexpr const char* kElemSetStub = "__bronze_elem_set_keyed";

// Builds the stubs (once, before any body) unless the build counts every
// computed access in the helpers, and tells the property lowering which.
void emit_elem_stubs(IlLowering* lowering, Module& mod);

}  // namespace il2mir

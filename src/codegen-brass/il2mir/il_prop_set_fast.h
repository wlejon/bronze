#pragma once

// kPropSetStub (il_property.h): a site's write answered from its way 0 with
// no call (an own data slot, or the add one shape above the receiver's), else
// by a call of kPropSetSlowStub. il_prop_set_fast.cpp says why it is written
// apart from the full write stub.

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

// Builds the stub into `fn`, (obj i64, key i32, val i64, site ptr, strict
// i32) -> void.
void build_prop_set_fast_stub(Function* fn);

}  // namespace il2mir

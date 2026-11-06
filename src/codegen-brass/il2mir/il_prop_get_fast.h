#pragma once

// kPropGetStub (il_property.h): a site's read answered from its cache with
// no call, else by a call of kPropGetSlowStub. il_prop_get_fast.cpp says why
// it is written apart from the full read stub.

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

// Builds the stub into `fn`, (obj i64, key i32, site ptr) -> i64. With
// `to_front`, a hit in the thread's megamorphic table is also installed at
// the site's way 0, the other ways moving down one (kPropGetFrontStub).
void build_prop_get_fast_stub(Function* fn, bool to_front = false);

}  // namespace il2mir

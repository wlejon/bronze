#pragma once

// `X.isArray(v)` answered at the site while its method cache holds the
// global constructor static `Array.isArray` (il_is_array.cpp).

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <vector>

namespace il2mir {

class IlLowering;

// The site's answer, or nullptr when the call is not one this lowers (another
// key, no arguments, a site that runs once or has no method stub). `boxed_args`
// are Value bits; everything the inline test does not answer goes through
// lower_method_call_ic with them.
Value* lower_is_array_call(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* recv,
                           const std::vector<Value*>& boxed_args, Value* site, bool poly_recv);

}  // namespace il2mir

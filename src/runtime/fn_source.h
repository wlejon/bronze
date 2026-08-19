#pragma once

// The source text of every compiled function, keyed by its call-wrapper
// address — what 20.2.3.5 Function.prototype.toString returns.
//
// Keyed by CODE and not by function object because the two are not the same
// count: one body compiled once can back a thousand closures, and the text is
// a fact about the body. So a closure created in a loop carries nothing, and
// the whole mechanism costs one map probe, at the moment a program actually
// asks a function what it looks like.

#include <cstdint>
#include <string_view>

#include "abi/bronze_abi.h"

namespace bronze::runtime {

// The text of the function whose call wrapper is `code`, or an empty view
// when nothing registered one. Points into the module's read-only data and
// stays valid for the life of the image.
std::string_view rtFunctionSourceText(const void* code);

}  // namespace bronze::runtime

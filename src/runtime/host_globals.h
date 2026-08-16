#pragma once

#include <string>

#include "runtime/value.h"

// The host-global registry: names an EMBEDDING host has provided values for,
// consulted by `bronze_global_get` after the builtin ladder and before its
// fatal. This is the runtime half of `--host-globals`: lowering admits the
// names onto its provided-globals list at compile time, and the host registers
// the values before `bronze_main` runs.
//
// Its own header rather than a line among the runtime's internal ones, because
// it has a caller OUTSIDE src/runtime: the embed module wraps
// `rtRegisterHostGlobal` for hosts, and the `rt_*.h` headers are the runtime's
// internal surface. The registry itself
// still lives in rt_state.cpp with the other root sources — embed only calls
// it, and the runtime never learns the embed module exists.

namespace bronze::runtime {

// Register (or replace) a host global. The value is rooted by the registry's
// root source, so it survives collections for the life of the process. A name
// that collides with a builtin is unreachable: `bronze_global_get` asks the
// builtins first, deliberately — a host must not be able to swap out `Math`
// under compiled code that was optimized against it.
void rtRegisterHostGlobal(const std::string& name, Value value);

// The registered value for `name`, through the out parameter. False when the
// host never registered the name — which is not the same as a registered
// `undefined`, and why this is not "return undefined for a miss".
bool rtHostGlobalLookup(const std::string& name, Value& out);

}  // namespace bronze::runtime

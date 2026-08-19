#pragma once

#include <functional>
#include <span>
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

// ---- CreateDynamicFunction, delegated ---------------------------------------
//
// `new Function(src)` is out of scope for an AOT compiler and the four dynamic
// constructors refuse it by name (builtin_function.cpp, builtin_function_kinds
// .cpp). That refusal is right for bronze ALONE and wrong for bronze inside a
// host that already runs an interpreter: bro embeds one, and a page it loads
// may hand a compiled program a function it built from a string — three.js's
// editor compiles every user script that way. Such a host can answer 27.3.1.1
// itself, so it is given the chance to.
//
// The hook is NOT a way to make bronze evaluate anything. It hands the host the
// arguments and takes back whatever callable the host produced; the runtime
// neither parses the source nor learns what the result is made of. With no hook
// installed the refusal stands exactly as before, which is what keeps a
// standalone bronze program honest about the feature it does not have.
//
// `kind` says WHICH constructor was called, because the four are distinct
// intrinsics and a host that answers `Function` must not silently answer
// `AsyncFunction` too.
enum class DynamicFunctionKind : uint32_t {
    Ordinary = 0,
    Generator = 1,
    Async = 2,
    AsyncGenerator = 3,
};

// The arguments are 27.3.1.1's: zero or more parameter lists followed by a
// body, all unconverted — ToString is the host's to perform, because a host
// that refuses a non-string argument wants to say so in its own words. `args`
// points at ROOTED slots (rt_roots.h RootedArgs::data), so the span stays
// current across anything the host allocates.
using DynamicFunctionHost =
    std::function<Value(DynamicFunctionKind kind, std::span<const Value> args)>;

// Install (or clear, with an empty function) the host's answer. Process-wide
// and set once at startup, like the host globals beside it.
void rtSetDynamicFunctionHost(DynamicFunctionHost host);

// The installed answer, or an empty std::function when there is none — which
// is the signal to refuse.
const DynamicFunctionHost& rtDynamicFunctionHost();

}  // namespace bronze::runtime

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

// ---- lazy host globals --------------------------------------------------------
//
// A name the host registers now and fills only when a program first reads it:
// the registry lists it (so compiled programs see it on the manifest, and
// `typeof name` answers), and the first lookup runs `init`, which registers
// the real value through rtRegisterHostGlobal. A host's whole wrapper module
// can wait that way until a program touches one of its names. Several names
// may share one init; init must be idempotent, since forcing any one of them
// runs it, and the others' pending entries are dropped as init registers them.
//
// On a realm's global object a pending name is an accessor whose getter forces
// it and whose setter forces it and then registers the assigned value (a
// module's `globalThis.X = ...` while its init runs). The accessor pair is
// made by the hook the embed layer installs (rtSetLazyGlobalAccessorHook):
// building a host function is the embed module's business.
//
// A lookup that forces a name the init did not register answers undefined.
void rtRegisterLazyHostGlobal(const std::string& name, std::function<void()> init);
// Run the pending init for `name`, if it still has one.
void rtForceLazyHostGlobal(const std::string& name);
// Whether `name` is registered, eager or pending, without forcing it.
bool rtHostGlobalRegistered(const std::string& name);
// Whether an init is running on this thread: a module entry run from one does
// not drain the microtask queue, whose jobs belong to the program that
// touched the name.
bool rtInLazyGlobalInit();
// Run `init` as a lazy global's init runs: for a host deferring work behind
// something other than a global name (a namespace accessor).
void rtRunAsLazyInit(const std::function<void()>& init);
// Returns the global object as it is after the define, which may have moved
// it (a slot block that grows).
using LazyGlobalAccessorHook = Value (*)(Value globalObject, const std::string& name);
void rtSetLazyGlobalAccessorHook(LazyGlobalAccessorHook hook);
LazyGlobalAccessorHook rtLazyGlobalAccessorHook();

// The builtins a host's registration REPLACES rather than loses to: namespaces
// compiled code calls into but is never optimized against, where a host may
// carry the fuller implementation. `performance` (a browser host's clock: bro's
// follows virtual time in headless) and `Intl` (bronze's own is a minimal
// subset; bro registers the complete one: NumberFormat grouping, formatToParts,
// PluralRules, RelativeTimeFormat, ...). `bronze_global_get` and the embed
// API's `globalValue` both ask this first, so they cannot answer differently.
inline bool rtHostShadowsBuiltin(const std::string& name) {
    return name == "performance" || name == "Intl";
}

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
void rtSetDefaultDynamicFunctionHost(DynamicFunctionHost host);

// The installed answer, or an empty std::function when there is none — which
// is the signal to refuse.
const DynamicFunctionHost& rtDynamicFunctionHost();

// ---- eval, delegated --------------------------------------------------------
//
// `eval` is `Function`'s sibling (19.2.1): a global function whose argument is
// SOURCE TEXT compiled at run time. bronze provides it as a real global — so
// `typeof eval` answers, `const e = eval` is a value, and the indirect
// spelling calls the same object — whose body delegates to this hook exactly
// as the four dynamic-function constructors delegate to theirs, and refuses
// with a catchable TypeError when no host installed one.
//
// What a host CANNOT be handed is the caller's scope: an AOT-compiled frame
// has no environment record to reify. Both spellings therefore get the
// INDIRECT semantics — the source evaluates against the global environment —
// which is 19.2.1's own behavior for every call that is not syntactically
// direct, and the only honest contract a compiled program can offer. The
// lowering warns at a syntactically direct call site, since source that reads
// the caller's locals would diverge silently.
//
// The runtime performs 19.2.1 step 2 itself (a non-string argument comes
// straight back, never reaching the hook), so the hook only ever sees source
// text. The argument is a ROOTED slot, current across anything the hook
// allocates.
using DynamicEvalHost = std::function<Value(Value source)>;

// Install (or clear, with an empty function) the host's answer. Process-wide
// and set once at startup, like the host globals beside it.
void rtSetDynamicEvalHost(DynamicEvalHost host);
void rtSetDefaultDynamicEvalHost(DynamicEvalHost host);

// The installed answer, or an empty std::function when there is none — which
// is the signal to refuse.
const DynamicEvalHost& rtDynamicEvalHost();

// ---- import(), delegated ----------------------------------------------------
//
// The module graph answers every `import()` whose specifier it could read at
// compile time — a string literal, or a template literal it could glob — and
// those calls never reach the runtime. What does reach it is the rest: a
// specifier that was a variable, or a template bounded by nothing. A
// standalone program has no answer for those and the call rejects; a host that
// can load a module at run time may install one here and is handed the
// specifier (a ROOTED slot, current across anything the host allocates,
// unconverted — ToString is the host's) and the importing module's URL, the
// same text its `import.meta.url` reads, which is what a relative specifier is
// resolved against. What the host returns is the call's value, uninspected;
// the program is awaiting it, so a promise is the honest shape.
using DynamicImportHost = std::function<Value(Value specifier, const std::string& referrerUrl)>;

// Install (or clear, with an empty function) the host's answer. Process-wide
// and set once at startup, like the host globals beside it.
void rtSetDynamicImportHost(DynamicImportHost host);

// The installed answer, or an empty std::function when there is none — which
// is the signal to reject.
const DynamicImportHost& rtDynamicImportHost();

}  // namespace bronze::runtime

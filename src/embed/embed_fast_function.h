#pragma once

// The FAST host function: a native method called through a plain function
// pointer, for the host entry points a program calls at render-loop rates
// (bro's WebGL binding is ~15k calls a frame). makeFunction (embed.h) stays
// the general API; this is the same function object with a cheaper call.
//
// What a call through makeFunction pays and this one does not:
//
//  * the std::function: one indirect call through a type-erased invoker, on
//    top of the trampoline's own. Here the trampoline calls the function
//    pointer the host registered, and the host's callable is inlined into it
//    (the template overload below generates that thunk);
//  * the shadow-stack root frame: a RootValueBlock for the receiver and the
//    arguments, a ShadowStackFrame opened, the span registered, the frame and
//    block closed again. Here the arguments are copied into a plain array in
//    the trampoline's own stack frame and nothing is registered;
//  * the checked env read: the brand, slot-count and realm checks of
//    handleDataAnyRealm. The trampoline reads the data slot of the env cell
//    it made itself.
//
// THE CONTRACT, which is the NativeFn one (embed.h) with one difference in
// how it is met:
//
//  * `args` points at a COPY of the arguments in the trampoline's native
//    stack frame. By the GC contract (embed.h, brass docs/gc_contract.md
//    2.5) every word of the thread's stack is read conservatively before a
//    collection, and an object a stack word names is pinned in place (young)
//    or marked (old) — so the copy stays CURRENT and its objects alive across
//    anything the callback does, allocations and nested calls into JS
//    included, exactly as NativeFn's rooted span does. A callback may
//    therefore allocate, return a fresh object, string or array, and read its
//    arguments after doing so. (Calls with more than kFastInlineArgs
//    arguments take the rooted path instead, transparently.)
//  * `thisValue` is a plain copy, current as a local, as with NativeFn.
//  * NO SHADOW-STACK FRAME is opened for the callback. Everything on the
//    embed surface opens its own where it needs one, so a callback that
//    calls only embed API is unaffected; a callback that reaches past it to
//    runtime::Rooted<> must open its own ShadowStackFrame first.
//  * Throwing is NativeFn's: call a throw helper (embed::throwTypeError, ...)
//    and return its result. A C++ exception escaping the callback becomes the
//    call's JS error exactly as on the NativeFn path (std::bad_alloc the
//    runtime's out-of-memory throw, any other std::exception an Error
//    carrying its what()). The handler is table-driven, so it costs the call nothing until
//    something throws.
//  * The function object is the same kind makeFunction makes: callable from
//    every realm, `length`/`name` filled the same way, NEEDS_ENV so the call
//    IC passes the env cell. `data` is released by `dataDtor` (when not
//    null) once the function object has been collected.

#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "embed/embed.h"

namespace bronze::embed {

// The receiver and the arguments as a span over the stack copy described
// above. `data` is the pointer registered with the function.
using FastNativeFn = Value (*)(void* data, Value thisValue, std::span<const Value> args);

// Arguments up to this count are copied onto the trampoline's stack; a call
// with more takes the rooted (RootValueBlock) path. Every WebGL entry point
// fits.
inline constexpr uint32_t kFastInlineArgs = 16;

// Wrap `fn` (+ `data`) into a bronze function object. `name` empty leaves the
// function unnamed, as makeFunction(fn, arity) does. ALLOCATES.
BRONZE_EMBED_API Value makeFastFunction(FastNativeFn fn, void* data, HandleDestructor dataDtor,
                                        uint32_t arity, std::string_view name = {});

// Any callable `Value(Value thisValue, std::span<const Value> args)` — the
// NativeFn signature, so a NativeFn lambda moves over unchanged — boxed on the
// C++ heap (not at all when it is a captureless lambda) and called through a
// thunk the compiler inlines it into. ALLOCATES.
template <typename F>
Value makeFastFunction(F&& f, uint32_t arity, std::string_view name = {}) {
    using Fn = std::decay_t<F>;
    static_assert(std::is_invocable_r_v<Value, Fn&, Value, std::span<const Value>>,
                  "a fast host function is callable as Value(Value, std::span<const Value>)");
    if constexpr (std::is_empty_v<Fn> && std::is_default_constructible_v<Fn>) {
        (void)f;
        return makeFastFunction(
            [](void*, Value self, std::span<const Value> args) -> Value {
                Fn fn{};
                return fn(self, args);
            },
            nullptr, nullptr, arity, name);
    } else {
        auto* boxed = new Fn(std::forward<F>(f));
        return makeFastFunction(
            [](void* d, Value self, std::span<const Value> args) -> Value {
                return (*static_cast<Fn*>(d))(self, args);
            },
            boxed, [](void* d) { delete static_cast<Fn*>(d); }, arity, name);
    }
}

}  // namespace bronze::embed

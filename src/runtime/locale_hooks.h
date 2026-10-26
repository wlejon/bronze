#pragma once

// Host implementations of the locale-sensitive builtin methods.
//
// ECMA-402 redefines Number.prototype.toLocaleString, the Date toLocale*String
// methods and String.prototype.localeCompare in terms of Intl. bronze ships
// the ECMA-262 fallbacks; a host carrying a full Intl (bro's host_intl.cpp)
// gives the real ones here instead of assigning them onto the prototypes.
// It has to: after lockdown (lockdown.h) the prototypes are frozen, and a host
// patching a shared intrinsic is exactly what lockdown exists to rule out.
// The builtin stays the same function object; only its body defers.
//
// Per thread, like every runtime table: a hook holds a host closure, set by
// the host on each thread that runs its Intl. A hook receives the method's
// receiver and arguments as the builtin got them, under the embed native
// contract (its raw Values are good until its first allocation).

#include <cstdint>
#include <functional>
#include <span>

#include "runtime/value.h"

namespace bronze::runtime {

enum class LocaleHook : uint8_t {
    NumberToLocaleString,
    DateToLocaleString,
    DateToLocaleDateString,
    DateToLocaleTimeString,
    StringLocaleCompare,
    Count,
};

using LocaleHookFn = std::function<Value(Value self, std::span<const Value> args)>;

// Install (or, with an empty function, remove) the calling thread's hook.
void rtSetLocaleHook(LocaleHook which, LocaleHookFn fn);

// Run the calling thread's hook for `which` if one is set, leaving its answer
// in `out`; false when there is none and the builtin answers itself.
bool rtRunLocaleHook(LocaleHook which, uint64_t thisBits, uint32_t argc, const uint64_t* argv,
                     uint64_t& out);

}  // namespace bronze::runtime

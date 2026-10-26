#include "runtime/locale_hooks.h"

#include <array>
#include <cstddef>
#include <utility>

#include "runtime/gc.h"
#include "runtime/rt_roots.h"

namespace bronze::runtime {

namespace {

constexpr size_t kHookCount = static_cast<size_t>(LocaleHook::Count);

// Leaked, as every runtime table is: a thread_local destructor would run the
// host's closures after the thread's runtime has gone.
std::array<LocaleHookFn, kHookCount>& hooks() {
    static thread_local auto* table = new std::array<LocaleHookFn, kHookCount>();
    return *table;
}

}  // namespace

void rtSetLocaleHook(LocaleHook which, LocaleHookFn fn) {
    const auto i = static_cast<size_t>(which);
    if (i >= kHookCount) return;
    hooks()[i] = std::move(fn);
}

bool rtRunLocaleHook(LocaleHook which, uint64_t thisBits, uint32_t argc, const uint64_t* argv,
                     uint64_t& out) {
    const auto i = static_cast<size_t>(which);
    if (i >= kHookCount) return false;
    LocaleHookFn& fn = hooks()[i];
    if (!fn) return false;
    // embed's host-function prologue (embed_function.cpp): the arguments in
    // rooted slots before the host can allocate, read through a span the
    // collector keeps current.
    ShadowStackFrame frame;
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    out = fn(self.get(), std::span<const Value>(args.data(), args.count())).rawBits();
    return true;
}

}  // namespace bronze::runtime

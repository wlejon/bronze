#pragma once

// For tests that assert an object dies, moves or is promoted at a given
// collection. brass reads the collecting thread's stack conservatively and
// keeps (and pins in place) every object a word of it names, stale words of
// dead frames included (brass docs/gc_contract.md 2.5). A Rooted<> on the
// stack is such a word: its slot is in the frame. Such a test makes the
// object in a function of its own that is never inlined (inOwnFrame,
// fromOwnFrame), keeps it where no stack word names it (a root slot on the
// C++ heap: heapRooted, or its address hidden), and calls scrubStack()
// before collecting, so the dead frames' words are gone. A frame called after
// another at the same depth sits where it was, and the slots it never writes
// keep its words: scrubStack() between the two when the second one collects.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#if defined(_MSC_VER) && !defined(__clang__)
#define BRONZE_TEST_NOINLINE __declspec(noinline)
#else
#define BRONZE_TEST_NOINLINE __attribute__((noinline))
#endif

namespace bronze::test {

// Overwrites the stack below the caller's frame, where the frames it called
// left their words.
BRONZE_TEST_NOINLINE inline void scrubStack() {
    volatile uint64_t words[4096];
    for (size_t i = 0; i < 4096; ++i) words[i] = 0;
    (void)words[0];
}

// A word kept in a local without naming its object: every bit flipped against
// a pattern, so no 48-bit address of the heap is left in it.
inline constexpr uint64_t kHideMask = 0x5A5A5A5A5A5A5A5AULL;
inline constexpr uint64_t hideBits(uint64_t bits) noexcept { return bits ^ kHideMask; }
// Not inline: the compiler would be free to compute the word early and keep
// it in a callee-saved register across a collection.
BRONZE_TEST_NOINLINE inline uint64_t unhideBits(uint64_t hidden) noexcept { return hidden ^ kHideMask; }

// Runs `f` in a frame below the caller's, never inlined into it.
template <class F>
BRONZE_TEST_NOINLINE void inOwnFrame(F&& f) {
    std::forward<F>(f)();
}

// The same, returning what `f` returns (a count, a hidden word: nothing that
// names an object).
template <class F>
BRONZE_TEST_NOINLINE auto fromOwnFrame(F&& f) {
    return std::forward<F>(f)();
}

// A root whose slot lives on the C++ heap, so no stack word holds what it
// roots (RootedT is the runtime's Rooted<T>; the caller's frame must outlive
// it, as for any Rooted).
template <class RootedT, class... Args>
std::unique_ptr<RootedT> heapRooted(Args&&... args) {
    return std::make_unique<RootedT>(std::forward<Args>(args)...);
}

}  // namespace bronze::test

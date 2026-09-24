// A portable std::atomic_ref subset.
//
// std::atomic_ref is C++20, but Apple's libc++ only ships it from Xcode 16
// (LLVM 19); the macos-14 runners that bro and its siblings build on do not
// have it. Where the library provides it this is a thin alias; elsewhere the
// same operations go through the GCC/Clang __atomic builtins, which act on a
// plain object in place exactly as atomic_ref does.
#pragma once

#include <atomic>
#include <version>

namespace bronze::runtime {

#if defined(__cpp_lib_atomic_ref)

template <typename T>
using AtomicRef = std::atomic_ref<T>;

#else

template <typename T>
class AtomicRef {
public:
    explicit AtomicRef(T& obj) : ptr_(&obj) {}

    T load(std::memory_order order = std::memory_order_seq_cst) const {
        return __atomic_load_n(ptr_, toBuiltin(order));
    }

    void store(T value, std::memory_order order = std::memory_order_seq_cst) const {
        __atomic_store_n(ptr_, value, toBuiltin(order));
    }

    bool compare_exchange_strong(T& expected, T desired,
                                 std::memory_order order = std::memory_order_seq_cst) const {
        return __atomic_compare_exchange_n(ptr_, &expected, desired, false, toBuiltin(order),
                                           toBuiltin(failureOrder(order)));
    }

private:
    static constexpr int toBuiltin(std::memory_order order) {
        switch (order) {
            case std::memory_order_relaxed: return __ATOMIC_RELAXED;
            case std::memory_order_consume: return __ATOMIC_CONSUME;
            case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
            case std::memory_order_release: return __ATOMIC_RELEASE;
            case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
            default: return __ATOMIC_SEQ_CST;
        }
    }

    // The failure half of a CAS may not release: std::atomic_ref derives it
    // the same way from a single order.
    static constexpr std::memory_order failureOrder(std::memory_order order) {
        if (order == std::memory_order_acq_rel) return std::memory_order_acquire;
        if (order == std::memory_order_release) return std::memory_order_relaxed;
        return order;
    }

    T* ptr_;
};

#endif

}  // namespace bronze::runtime

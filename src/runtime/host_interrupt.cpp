// Host interrupts (host_interrupt.h).

#include "runtime/host_interrupt.h"

#include "runtime/tls_block.h"

#include <atomic>
#include <cstdint>

namespace bronze::runtime {

struct HostInterrupt {
    bronze_tls_block* tls = nullptr;
    std::atomic<HostInterruptFn> fn{nullptr};
    std::atomic<void*> arg{nullptr};
    std::atomic<bool> pending{false};
};

namespace {

thread_local HostInterrupt* t_host = nullptr;
thread_local bool t_inHost = false;

// What the function may use of the stack: a window system's message dispatch
// goes through the OS's own frames.
constexpr uintptr_t kHostStackNeed = 256 * 1024;

}  // namespace

HostInterrupt* rtRegisterHostInterrupt(HostInterruptFn fn, void* arg) {
    if (!t_host) {
        t_host = new HostInterrupt();  // never freed: requests may outlive the thread
        t_host->tls = rtTls();
    }
    t_host->arg.store(arg, std::memory_order_relaxed);
    t_host->fn.store(fn, std::memory_order_release);
    return t_host;
}

void rtUnregisterHostInterrupt() {
    if (!t_host) return;
    t_host->fn.store(nullptr, std::memory_order_release);
    t_host->pending.store(false, std::memory_order_relaxed);
}

void rtRequestHostInterrupt(HostInterrupt* h) noexcept {
    if (!h || !h->fn.load(std::memory_order_acquire)) return;
    h->pending.store(true, std::memory_order_release);
    rtArmInterrupt(h->tls);
}

void rtServiceHostInterrupt() {
    HostInterrupt* h = t_host;
    if (!h || t_inHost || !h->pending.load(std::memory_order_acquire)) return;
    const uintptr_t limit = rtStackLimit();
    char marker = 0;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(&marker);
    // Short of stack: left pending, for a poll with more room (the host's
    // next request arms it again).
    if (limit != 0 && (sp <= limit || sp - limit < kHostStackNeed)) return;
    h->pending.store(false, std::memory_order_relaxed);
    const HostInterruptFn fn = h->fn.load(std::memory_order_acquire);
    if (!fn) return;
    t_inHost = true;
    fn(h->arg.load(std::memory_order_relaxed));
    t_inHost = false;
}

}  // namespace bronze::runtime
